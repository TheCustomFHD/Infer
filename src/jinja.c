/* jinja.c -- Jinja2 subset interpreter. See jinja.h.
 *
 * Structure: a recursive-descent expression parser over a template that
 * is walked linearly. Blocks ({% if %}, {% for %}, {% macro %}) are
 * handled by scanning forward for the matching tag, so no AST is built
 * -- this keeps the memory footprint tiny, which matters on the target.
 *
 * The cost of that choice is that a {% for %} body is re-scanned once
 * per iteration. Chat templates are a few kilobytes and loop over a
 * handful of messages, so this is irrelevant in practice and buys a
 * much simpler, smaller implementation.
 *
 * ANSI C (C89).
 */

#include "jinja.h"
#include "jinja_priv.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

static jj_val *nv(int t) {
    jj_val *v = (jj_val *) xcalloc(1, sizeof(jj_val));
    v->type = t;
    return v;
}

jj_val *jj_none(void)        { return nv(JJ_NONE); }
jj_val *jj_bool(int b)       { jj_val *v = nv(JJ_BOOL); v->num = b ? 1 : 0; return v; }
jj_val *jj_num(double d)     { jj_val *v = nv(JJ_NUM); v->num = d; return v; }

jj_val *jj_strn(const char *s, size_t n) {
    jj_val *v = nv(JJ_STR);
    v->str = (char *) xmalloc(n + 1);
    if (n) memcpy(v->str, s, n);
    v->str[n] = '\0';
    return v;
}
jj_val *jj_str(const char *s) { return jj_strn(s ? s : "", s ? strlen(s) : 0); }
jj_val *jj_list(void)        { return nv(JJ_LIST); }
jj_val *jj_dict(void)        { return nv(JJ_DICT); }

static void grow(jj_val *c) {
    if (c->n < c->cap) return;
    c->cap = c->cap ? c->cap * 2 : 8;
    c->items = (jj_val **) xrealloc(c->items, sizeof(jj_val *) * (size_t) c->cap);
    if (c->type == JJ_DICT) {
        c->keys = (char **) xrealloc(c->keys, sizeof(char *) * (size_t) c->cap);
    }
}

void jj_list_add(jj_val *l, jj_val *v) {
    grow(l);
    l->items[l->n++] = v;
}

void jj_dict_set(jj_val *d, const char *k, jj_val *v) {
    int i;
    for (i = 0; i < d->n; i++) {
        if (strcmp(d->keys[i], k) == 0) {
            jj_free(d->items[i]);
            d->items[i] = v;
            return;
        }
    }
    grow(d);
    d->keys[d->n] = xstrdup(k);
    d->items[d->n] = v;
    d->n++;
}

jj_val *jj_dict_get(const jj_val *d, const char *k) {
    int i;
    if (!d || d->type != JJ_DICT) return NULL;
    for (i = 0; i < d->n; i++) {
        if (strcmp(d->keys[i], k) == 0) return d->items[i];
    }
    return NULL;
}

void jj_free(jj_val *v) {
    int i;
    if (!v) return;
    if (v->str) free(v->str);
    for (i = 0; i < v->n; i++) {
        if (v->items) jj_free(v->items[i]);
        if (v->keys && v->keys[i]) free(v->keys[i]);
    }
    if (v->items) free(v->items);
    if (v->keys) free(v->keys);
    free(v);
}

jj_val *jj_clone(const jj_val *v) {
    jj_val *c;
    int i;
    if (!v) return jj_none();
    switch (v->type) {
        case JJ_NONE: return jj_none();
        case JJ_BOOL: return jj_bool(v->num != 0);
        case JJ_NUM:  return jj_num(v->num);
        case JJ_STR:  return jj_str(v->str);
        default:
            c = nv(v->type);
            for (i = 0; i < v->n; i++) {
                grow(c);
                if (v->type == JJ_DICT) c->keys[c->n] = xstrdup(v->keys[i]);
                c->items[c->n] = jj_clone(v->items[i]);
                c->n++;
            }
            return c;
    }
}

int jj_truthy(const jj_val *v) {
    if (!v) return 0;
    switch (v->type) {
        case JJ_NONE: return 0;
        case JJ_BOOL:
        case JJ_NUM:  return v->num != 0.0;
        case JJ_STR:  return v->str && v->str[0];
        default:      return v->n > 0;
    }
}

/* Render a value the way Jinja's {{ }} does. */
void jj_tostr(const jj_val *v, strbuf *b) {
    char tmp[64];
    int i;
    if (!v) return;
    switch (v->type) {
        case JJ_NONE: sb_puts(b, "None"); break;
        case JJ_BOOL: sb_puts(b, v->num != 0 ? "True" : "False"); break;
        case JJ_NUM:
            if (v->num == floor(v->num) && fabs(v->num) < 1e15) {
                sprintf(tmp, "%ld", (long) v->num);
            } else {
                sprintf(tmp, "%g", v->num);
            }
            sb_puts(b, tmp);
            break;
        case JJ_STR: sb_puts(b, v->str ? v->str : ""); break;
        case JJ_LIST:
            sb_puts(b, "[");
            for (i = 0; i < v->n; i++) {
                if (i) sb_puts(b, ", ");
                jj_tostr(v->items[i], b);
            }
            sb_puts(b, "]");
            break;
        default:
            sb_puts(b, "{");
            for (i = 0; i < v->n; i++) {
                if (i) sb_puts(b, ", ");
                sb_puts(b, "'"); sb_puts(b, v->keys[i]); sb_puts(b, "': ");
                jj_tostr(v->items[i], b);
            }
            sb_puts(b, "}");
            break;
    }
}

/* JSON serialisation for the |tojson filter. */
void jj_tojson(const jj_val *v, strbuf *b) {
    char tmp[64];
    int i;
    if (!v) { sb_puts(b, "null"); return; }
    switch (v->type) {
        case JJ_NONE: sb_puts(b, "null"); break;
        case JJ_BOOL: sb_puts(b, v->num != 0 ? "true" : "false"); break;
        case JJ_NUM:
            if (v->num == floor(v->num) && fabs(v->num) < 1e15) {
                sprintf(tmp, "%ld", (long) v->num);
            } else {
                sprintf(tmp, "%g", v->num);
            }
            sb_puts(b, tmp);
            break;
        case JJ_STR:
            sb_puts(b, "\"");
            sb_json_escape(b, v->str ? v->str : "", v->str ? strlen(v->str) : 0);
            sb_puts(b, "\"");
            break;
        case JJ_LIST:
            sb_puts(b, "[");
            for (i = 0; i < v->n; i++) {
                if (i) sb_puts(b, ", ");
                jj_tojson(v->items[i], b);
            }
            sb_puts(b, "]");
            break;
        default: {
            /* Jinja's |tojson sorts object keys (json.dumps has
             * sort_keys=True in its policy). Match that exactly, or
             * prompts differ byte-for-byte from every other runtime. */
            int *ord = (int *) xmalloc(sizeof(int) * (size_t) (v->n ? v->n : 1));
            int j, k;
            for (i = 0; i < v->n; i++) ord[i] = i;
            for (i = 1; i < v->n; i++) {
                int key = ord[i];
                j = i - 1;
                while (j >= 0 && strcmp(v->keys[ord[j]], v->keys[key]) > 0) {
                    ord[j + 1] = ord[j];
                    j--;
                }
                ord[j + 1] = key;
            }
            sb_puts(b, "{");
            for (i = 0; i < v->n; i++) {
                k = ord[i];
                if (i) sb_puts(b, ", ");
                sb_puts(b, "\"");
                sb_json_escape(b, v->keys[k], strlen(v->keys[k]));
                sb_puts(b, "\": ");
                jj_tojson(v->items[k], b);
            }
            sb_puts(b, "}");
            free(ord);
            break;
        }
    }
}
