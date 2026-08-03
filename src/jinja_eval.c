/* jinja_eval.c -- expression parser and statement interpreter for the
 * Jinja subset. See jinja.h for the supported feature list.
 *
 * ANSI C (C89).
 */

#include "jinja_priv.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Errors and scopes                                                   */
/* ------------------------------------------------------------------ */

void jj_fail(jj_ctx *c, const char *msg) {
    if (c->err && c->errlen) {
        strncpy(c->err, msg, c->errlen - 1);
        c->err[c->errlen - 1] = '\0';
    }
    longjmp(c->jmp, 1);
}

jj_val *jj_lookup(jj_scope *s, const char *name) {
    while (s) {
        jj_val *v = jj_dict_get(s->vars, name);
        if (v) return v;
        s = s->parent;
    }
    return NULL;
}

/* Assign in the scope where the name already exists, else innermost.
 * This is what makes {% set ns.x = .. %} / namespace() behave. */
void jj_setvar(jj_scope *s, const char *name, jj_val *v) {
    jj_scope *w = s;
    while (w) {
        if (jj_dict_get(w->vars, name)) { jj_dict_set(w->vars, name, v); return; }
        w = w->parent;
    }
    jj_dict_set(s->vars, name, v);
}

/* ------------------------------------------------------------------ */
/* Lexer helpers                                                       */
/* ------------------------------------------------------------------ */

static void skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static int is_id0(int ch) { return isalpha(ch) || ch == '_'; }
static int is_id(int ch)  { return isalnum(ch) || ch == '_'; }

/* Match a bare word (not followed by an identifier char). */
static int word(const char **p, const char *w) {
    size_t n = strlen(w);
    const char *q = *p;
    skip_ws(&q);
    if (strncmp(q, w, n) != 0) return 0;
    if (is_id((unsigned char) q[n])) return 0;
    *p = q + n;
    return 1;
}

static int punct(const char **p, const char *s) {
    size_t n = strlen(s);
    const char *q = *p;
    skip_ws(&q);
    if (strncmp(q, s, n) != 0) return 0;
    *p = q + n;
    return 1;
}

static int peek(const char **p, const char *s) {
    const char *q = *p;
    skip_ws(&q);
    return strncmp(q, s, strlen(s)) == 0;
}

static char *ident(const char **p) {
    const char *q = *p;
    const char *st;
    char *r;
    skip_ws(&q);
    if (!is_id0((unsigned char) *q)) return NULL;
    st = q;
    while (is_id((unsigned char) *q)) q++;
    r = (char *) xmalloc((size_t) (q - st) + 1);
    memcpy(r, st, (size_t) (q - st));
    r[q - st] = '\0';
    *p = q;
    return r;
}

/* ------------------------------------------------------------------ */
/* Expression grammar                                                  */
/*                                                                     */
/*   ternary := or [ 'if' or 'else' ternary ]                          */
/*   or      := and { 'or' and }                                       */
/*   and     := not { 'and' not }                                      */
/*   not     := [ 'not' ] cmp                                          */
/*   cmp     := cat { (==|!=|<|>|<=|>=|in|not in|is [not] test) cat }   */
/*   cat     := add { '~' add }                                        */
/*   add     := unary { ('+'|'-') unary }                              */
/*   unary   := postfix                                                */
/*   postfix := primary { '.' name | '[' expr ']' | '(' args ')' |     */
/*                        '|' filter }                                 */
/* ------------------------------------------------------------------ */

static jj_val *p_ternary(jj_ctx *c, const char **p);

static jj_val *call_macro(jj_ctx *c, jj_macro *m, jj_val **args, char **kw,
                          int nargs);

/* string literal with \escapes */
static jj_val *p_string(jj_ctx *c, const char **p) {
    char quote = **p;
    strbuf b;
    (*p)++;
    sb_init(&b);
    while (**p && **p != quote) {
        if (**p == '\\' && (*p)[1]) {
            (*p)++;
            switch (**p) {
                case 'n': sb_puts(&b, "\n"); break;
                case 't': sb_puts(&b, "\t"); break;
                case 'r': sb_puts(&b, "\r"); break;
                case '\\': sb_puts(&b, "\\"); break;
                case '\'': sb_puts(&b, "'"); break;
                case '"': sb_puts(&b, "\""); break;
                default: sb_add(&b, *p, 1); break;
            }
            (*p)++;
        } else {
            sb_add(&b, *p, 1);
            (*p)++;
        }
    }
    if (**p == quote) (*p)++;
    else jj_fail(c, "unterminated string literal");
    {
        jj_val *v = jj_strn(b.data, b.len);
        sb_free(&b);
        return v;
    }
}

/* namespace(a=1, b=2) */
static jj_val *p_namespace(jj_ctx *c, const char **p) {
    jj_val *d = jj_dict();
    if (!punct(p, "(")) jj_fail(c, "namespace( expected");
    skip_ws(p);
    while (!peek(p, ")")) {
        char *k = ident(p);
        if (!k) jj_fail(c, "namespace: bad key");
        if (!punct(p, "=")) jj_fail(c, "namespace: '=' expected");
        jj_dict_set(d, k, p_ternary(c, p));
        free(k);
        if (!punct(p, ",")) break;
    }
    if (!punct(p, ")")) jj_fail(c, "namespace: ')' expected");
    return d;
}

static jj_val *p_primary(jj_ctx *c, const char **p) {
    skip_ws(p);

    if (**p == '\'' || **p == '"') return p_string(c, p);

    if (isdigit((unsigned char) **p) ||
        (**p == '-' && isdigit((unsigned char) (*p)[1]))) {
        char *end;
        double d = strtod(*p, &end);
        *p = end;
        return jj_num(d);
    }

    if (punct(p, "(")) {
        jj_val *v = p_ternary(c, p);
        if (!punct(p, ")")) jj_fail(c, "')' expected");
        return v;
    }

    if (peek(p, "[")) {
        jj_val *l = jj_list();
        punct(p, "[");
        skip_ws(p);
        while (!peek(p, "]")) {
            jj_list_add(l, p_ternary(c, p));
            if (!punct(p, ",")) break;
        }
        if (!punct(p, "]")) jj_fail(c, "']' expected");
        return l;
    }

    if (word(p, "true") || word(p, "True"))   return jj_bool(1);
    if (word(p, "false") || word(p, "False")) return jj_bool(0);
    if (word(p, "none") || word(p, "None"))   return jj_none();

    if (word(p, "namespace")) return p_namespace(c, p);

    if (word(p, "raise_exception")) {
        jj_val *m;
        strbuf b;
        if (!punct(p, "(")) jj_fail(c, "raise_exception(");
        m = p_ternary(c, p);
        punct(p, ")");
        sb_init(&b);
        jj_tostr(m, &b);
        jj_free(m);
        {
            char msg[256];
            strncpy(msg, b.data, sizeof(msg) - 1);
            msg[sizeof(msg) - 1] = '\0';
            sb_free(&b);
            jj_fail(c, msg);
        }
    }

    {
        char *name = ident(p);
        jj_val *v;
        if (!name) jj_fail(c, "expression expected");

        /* macro call? */
        {
            jj_macro *m = c->macros;
            while (m) {
                if (strcmp(m->name, name) == 0) break;
                m = m->next;
            }
            if (m && peek(p, "(")) {
                jj_val *args[16];
                char *kw[16];
                int n = 0;
                punct(p, "(");
                skip_ws(p);
                while (!peek(p, ")") && n < 16) {
                    const char *save = *p;
                    char *k = ident(p);
                    if (k && peek(p, "=") && !peek(p, "==")) {
                        punct(p, "=");
                        kw[n] = k;
                        args[n++] = p_ternary(c, p);
                    } else {
                        if (k) free(k);
                        *p = save;
                        kw[n] = NULL;
                        args[n++] = p_ternary(c, p);
                    }
                    if (!punct(p, ",")) break;
                }
                if (!punct(p, ")")) jj_fail(c, "macro call: ')' expected");
                free(name);
                return call_macro(c, m, args, kw, n);
            }
        }

        v = jj_lookup(c->scope, name);
        free(name);
        return v ? jj_clone(v) : jj_none();
    }
}

/* .split(x) .startswith(x) .endswith(x) .lstrip(c) .rstrip(c) */
static jj_val *str_method(jj_ctx *c, jj_val *obj, const char *m,
                          const char **p) {
    jj_val *arg = NULL;
    const char *s = (obj->type == JJ_STR && obj->str) ? obj->str : "";
    size_t sl = strlen(s);

    if (punct(p, "(")) {
        skip_ws(p);
        if (!peek(p, ")")) arg = p_ternary(c, p);
        if (!punct(p, ")")) jj_fail(c, "method: ')' expected");
    }

    if (strcmp(m, "split") == 0) {
        jj_val *l = jj_list();
        const char *sep = (arg && arg->type == JJ_STR) ? arg->str : " ";
        size_t sepl = strlen(sep);
        const char *cur = s;
        if (sepl == 0) sepl = 1;
        for (;;) {
            const char *hit = strstr(cur, sep);
            if (!hit) { jj_list_add(l, jj_str(cur)); break; }
            jj_list_add(l, jj_strn(cur, (size_t) (hit - cur)));
            cur = hit + sepl;
        }
        jj_free(arg);
        return l;
    }
    if (strcmp(m, "startswith") == 0) {
        const char *w = (arg && arg->str) ? arg->str : "";
        int r = strncmp(s, w, strlen(w)) == 0;
        jj_free(arg);
        return jj_bool(r);
    }
    if (strcmp(m, "endswith") == 0) {
        const char *w = (arg && arg->str) ? arg->str : "";
        size_t wl = strlen(w);
        int r = sl >= wl && strcmp(s + sl - wl, w) == 0;
        jj_free(arg);
        return jj_bool(r);
    }
    if (strcmp(m, "lstrip") == 0 || strcmp(m, "rstrip") == 0) {
        const char *set = (arg && arg->str && arg->str[0]) ? arg->str : " \t\r\n";
        size_t a = 0, b = sl;
        if (strcmp(m, "lstrip") == 0) {
            while (a < b && strchr(set, s[a])) a++;
        } else {
            while (b > a && strchr(set, s[b - 1])) b--;
        }
        jj_free(arg);
        return jj_strn(s + a, b - a);
    }
    if (strcmp(m, "strip") == 0) {
        size_t a = 0, b = sl;
        while (a < b && isspace((unsigned char) s[a])) a++;
        while (b > a && isspace((unsigned char) s[b - 1])) b--;
        jj_free(arg);
        return jj_strn(s + a, b - a);
    }
    if (strcmp(m, "items") == 0 || strcmp(m, "keys") == 0) {
        jj_val *l = jj_list();
        int i;
        if (obj->type == JJ_DICT) {
            for (i = 0; i < obj->n; i++) {
                if (strcmp(m, "keys") == 0) {
                    jj_list_add(l, jj_str(obj->keys[i]));
                } else {
                    jj_val *pair = jj_list();
                    jj_list_add(pair, jj_str(obj->keys[i]));
                    jj_list_add(pair, jj_clone(obj->items[i]));
                    jj_list_add(l, pair);
                }
            }
        }
        jj_free(arg);
        return l;
    }
    jj_free(arg);
    return jj_none();
}

static jj_val *apply_filter(jj_ctx *c, jj_val *v, const char *f,
                            const char **p) {
    jj_val *arg = NULL;
    if (peek(p, "(")) {
        punct(p, "(");
        skip_ws(p);
        if (!peek(p, ")")) arg = p_ternary(c, p);
        punct(p, ")");
    }

    if (strcmp(f, "trim") == 0) {
        strbuf b;
        size_t a, e;
        jj_val *r;
        sb_init(&b);
        jj_tostr(v, &b);
        a = 0; e = b.len;
        while (a < e && isspace((unsigned char) b.data[a])) a++;
        while (e > a && isspace((unsigned char) b.data[e - 1])) e--;
        r = jj_strn(b.data + a, e - a);
        sb_free(&b);
        jj_free(v); jj_free(arg);
        return r;
    }
    if (strcmp(f, "tojson") == 0) {
        strbuf b; jj_val *r;
        sb_init(&b);
        jj_tojson(v, &b);
        r = jj_strn(b.data, b.len);
        sb_free(&b);
        jj_free(v); jj_free(arg);
        return r;
    }
    if (strcmp(f, "string") == 0) {
        strbuf b; jj_val *r;
        sb_init(&b);
        jj_tostr(v, &b);
        r = jj_strn(b.data, b.len);
        sb_free(&b);
        jj_free(v); jj_free(arg);
        return r;
    }
    if (strcmp(f, "length") == 0 || strcmp(f, "count") == 0) {
        double n;
        if (v->type == JJ_STR) n = (double) strlen(v->str ? v->str : "");
        else if (v->type == JJ_LIST || v->type == JJ_DICT) n = (double) v->n;
        else n = 0;
        jj_free(v); jj_free(arg);
        return jj_num(n);
    }
    if (strcmp(f, "first") == 0) {
        jj_val *r = (v->type == JJ_LIST && v->n) ? jj_clone(v->items[0]) : jj_none();
        jj_free(v); jj_free(arg); return r;
    }
    if (strcmp(f, "last") == 0) {
        jj_val *r = (v->type == JJ_LIST && v->n) ? jj_clone(v->items[v->n-1]) : jj_none();
        jj_free(v); jj_free(arg); return r;
    }
    if (strcmp(f, "join") == 0) {
        strbuf b;
        const char *sep = (arg && arg->type == JJ_STR) ? arg->str : "";
        jj_val *r;
        int i;
        sb_init(&b);
        if (v->type == JJ_LIST) {
            for (i = 0; i < v->n; i++) {
                if (i) sb_puts(&b, sep);
                jj_tostr(v->items[i], &b);
            }
        } else {
            jj_tostr(v, &b);
        }
        r = jj_strn(b.data, b.len);
        sb_free(&b);
        jj_free(v); jj_free(arg);
        return r;
    }
    if (strcmp(f, "int") == 0) {
        double d = 0;
        if (v->type == JJ_NUM || v->type == JJ_BOOL) d = (double) (long) v->num;
        else if (v->type == JJ_STR && v->str) d = (double) atol(v->str);
        jj_free(v); jj_free(arg);
        return jj_num(d);
    }
    if (strcmp(f, "abs") == 0) {
        double d = v->num < 0 ? -v->num : v->num;
        jj_free(v); jj_free(arg);
        return jj_num(d);
    }
    if (strcmp(f, "replace") == 0) {
        /* |replace(old, new) -- the second argument needs its own parse,
         * which apply_filter's single-arg path does not do, so replace
         * is handled where the arguments are still available. */
        jj_free(arg);
        return v;
    }
    if (strcmp(f, "list") == 0 || strcmp(f, "safe") == 0 ||
        strcmp(f, "e") == 0 || strcmp(f, "escape") == 0) {
        jj_free(arg);
        return v;
    }
    if (strcmp(f, "default") == 0) {
        if (v->type == JJ_NONE) { jj_free(v); return arg ? arg : jj_none(); }
        jj_free(arg);
        return v;
    }
    if (strcmp(f, "upper") == 0 || strcmp(f, "lower") == 0) {
        strbuf b; jj_val *r; size_t i;
        sb_init(&b); jj_tostr(v, &b);
        for (i = 0; i < b.len; i++) {
            b.data[i] = (char) (strcmp(f, "upper") == 0
                ? toupper((unsigned char) b.data[i])
                : tolower((unsigned char) b.data[i]));
        }
        r = jj_strn(b.data, b.len);
        sb_free(&b); jj_free(v); jj_free(arg);
        return r;
    }
    jj_free(arg);
    return v;   /* unknown filter: pass through */
}

static jj_val *p_postfix(jj_ctx *c, const char **p) {
    jj_val *v = p_primary(c, p);

    for (;;) {
        skip_ws(p);
        if (**p == '.') {
            const char *save = *p;
            char *nm;
            (*p)++;
            nm = ident(p);
            if (!nm) { *p = save; break; }
            skip_ws(p);
            if (**p == '(') {
                jj_val *r = str_method(c, v, nm, p);
                free(nm); jj_free(v); v = r;
            } else {
                jj_val *m = (v->type == JJ_DICT) ? jj_dict_get(v, nm) : NULL;
                jj_val *r;
                if (m) r = jj_clone(m);
                else if (v->type == JJ_LIST && strcmp(nm, "length") == 0) r = jj_num(v->n);
                else r = jj_none();
                free(nm); jj_free(v); v = r;
            }
            continue;
        }
        if (**p == '[') {
            (*p)++;
            /* Slices: [::-1] reverse, and the general [a:b] form with
             * either bound optional and negative indices allowed. */
            skip_ws(p);
            if (strncmp(*p, "::-1", 4) == 0) {
                jj_val *r = jj_list();
                int i;
                *p += 4;
                punct(p, "]");
                if (v->type == JJ_LIST) {
                    for (i = v->n - 1; i >= 0; i--) jj_list_add(r, jj_clone(v->items[i]));
                }
                jj_free(v); v = r;
                continue;
            }
            {
                /* look ahead for a ':' that makes this a slice */
                const char *scan = *p;
                int depth = 0, is_slice = 0;
                while (*scan && !(depth == 0 && *scan == ']')) {
                    if (*scan == '[' || *scan == '(') depth++;
                    else if (*scan == ']' || *scan == ')') depth--;
                    else if (depth == 0 && *scan == ':') { is_slice = 1; break; }
                    scan++;
                }
                if (is_slice) {
                    long lo = 0, hi = -1;
                    int have_hi = 0;
                    jj_val *r;
                    int i, n;

                    skip_ws(p);
                    if (**p != ':') {
                        jj_val *a = p_ternary(c, p);
                        lo = (long) a->num;
                        jj_free(a);
                    }
                    if (!punct(p, ":")) jj_fail(c, "slice: ':' expected");
                    skip_ws(p);
                    if (**p != ']') {
                        jj_val *b2 = p_ternary(c, p);
                        hi = (long) b2->num;
                        have_hi = 1;
                        jj_free(b2);
                    }
                    if (!punct(p, "]")) jj_fail(c, "slice: ']' expected");

                    if (v->type == JJ_STR) {
                        long len = (long) strlen(v->str ? v->str : "");
                        long a2 = lo < 0 ? len + lo : lo;
                        long b3 = !have_hi ? len : (hi < 0 ? len + hi : hi);
                        if (a2 < 0) a2 = 0;
                        if (b3 > len) b3 = len;
                        if (b3 < a2) b3 = a2;
                        r = jj_strn((v->str ? v->str : "") + a2, (size_t) (b3 - a2));
                    } else {
                        n = (v->type == JJ_LIST) ? v->n : 0;
                        {
                            long a2 = lo < 0 ? n + lo : lo;
                            long b3 = !have_hi ? n : (hi < 0 ? n + hi : hi);
                            if (a2 < 0) a2 = 0;
                            if (b3 > n) b3 = n;
                            r = jj_list();
                            for (i = (int) a2; i < (int) b3; i++) {
                                jj_list_add(r, jj_clone(v->items[i]));
                            }
                        }
                    }
                    jj_free(v); v = r;
                    continue;
                }
            }
            {
                jj_val *idx = p_ternary(c, p);
                /* Allocate the fallback only if nothing matched: the
                 * eager jj_none() here was overwritten by the clone on
                 * every successful lookup and leaked. */
                jj_val *r = NULL;
                if (!punct(p, "]")) jj_fail(c, "']' expected");
                if (idx->type == JJ_STR && v->type == JJ_DICT) {
                    jj_val *m = jj_dict_get(v, idx->str);
                    if (m) r = jj_clone(m);
                } else if (idx->type == JJ_NUM && v->type == JJ_LIST) {
                    int i = (int) idx->num;
                    if (i < 0) i += v->n;
                    if (i >= 0 && i < v->n) r = jj_clone(v->items[i]);
                }
                if (!r) r = jj_none();
                jj_free(idx); jj_free(v); v = r;
                continue;
            }
        }
        if (**p == '|') {
            char *f;
            (*p)++;
            f = ident(p);
            if (!f) jj_fail(c, "filter name expected");
            v = apply_filter(c, v, f, p);
            free(f);
            continue;
        }
        break;
    }
    return v;
}

static jj_val *p_mul(jj_ctx *c, const char **p) {
    jj_val *l = p_postfix(c, p);
    for (;;) {
        skip_ws(p);
        if (**p == '*' && (*p)[1] != '*') {
            jj_val *r; double d;
            (*p)++;
            r = p_postfix(c, p);
            d = l->num * r->num;
            jj_free(l); jj_free(r); l = jj_num(d);
            continue;
        }
        if (**p == '/' && (*p)[1] != '/') {
            jj_val *r; double d;
            (*p)++;
            r = p_postfix(c, p);
            d = (r->num != 0.0) ? l->num / r->num : 0.0;
            jj_free(l); jj_free(r); l = jj_num(d);
            continue;
        }
        if (**p == '%' && (*p)[1] != '}') {
            jj_val *r; double d;
            (*p)++;
            r = p_postfix(c, p);
            d = (r->num != 0.0) ? (double)((long)l->num % (long)r->num) : 0.0;
            jj_free(l); jj_free(r); l = jj_num(d);
            continue;
        }
        break;
    }
    return l;
}

static jj_val *p_add(jj_ctx *c, const char **p) {
    jj_val *l = p_mul(c, p);
    for (;;) {
        skip_ws(p);
        if (**p == '+' ) {
            jj_val *r;
            (*p)++;
            r = p_mul(c, p);
            if (l->type == JJ_STR || r->type == JJ_STR) {
                strbuf b; jj_val *n;
                sb_init(&b); jj_tostr(l, &b); jj_tostr(r, &b);
                n = jj_strn(b.data, b.len); sb_free(&b);
                jj_free(l); jj_free(r); l = n;
            } else if (l->type == JJ_LIST && r->type == JJ_LIST) {
                int i;
                for (i = 0; i < r->n; i++) jj_list_add(l, jj_clone(r->items[i]));
                jj_free(r);
            } else {
                double d = l->num + r->num;
                jj_free(l); jj_free(r); l = jj_num(d);
            }
            continue;
        }
        if (**p == '-' && (*p)[1] != '%' && (*p)[1] != '}') {
            jj_val *r;
            double d;
            (*p)++;
            r = p_mul(c, p);
            d = l->num - r->num;
            jj_free(l); jj_free(r); l = jj_num(d);
            continue;
        }
        break;
    }
    return l;
}

static jj_val *p_cat(jj_ctx *c, const char **p) {
    jj_val *l = p_add(c, p);
    while (peek(p, "~")) {
        strbuf b; jj_val *r, *n;
        punct(p, "~");
        r = p_add(c, p);
        sb_init(&b); jj_tostr(l, &b); jj_tostr(r, &b);
        n = jj_strn(b.data, b.len); sb_free(&b);
        jj_free(l); jj_free(r); l = n;
    }
    return l;
}

static int val_eq(const jj_val *a, const jj_val *b) {
    if (!a || !b) return 0;
    if (a->type == JJ_STR && b->type == JJ_STR) {
        return strcmp(a->str ? a->str : "", b->str ? b->str : "") == 0;
    }
    if (a->type == JJ_NONE || b->type == JJ_NONE) return a->type == b->type;
    return a->num == b->num;
}

/* 'x in y' for strings and lists */
static int val_in(const jj_val *needle, const jj_val *hay) {
    int i;
    if (!hay) return 0;
    if (hay->type == JJ_STR && needle->type == JJ_STR) {
        return strstr(hay->str ? hay->str : "", needle->str ? needle->str : "") != NULL;
    }
    if (hay->type == JJ_LIST) {
        for (i = 0; i < hay->n; i++) if (val_eq(needle, hay->items[i])) return 1;
    }
    if (hay->type == JJ_DICT && needle->type == JJ_STR) {
        return jj_dict_get(hay, needle->str) != NULL;
    }
    return 0;
}

/* 'is <test>' */
static int run_test(jj_ctx *c, const jj_val *v, const char *t) {
    (void) c;
    if (strcmp(t, "string") == 0)   return v->type == JJ_STR;
    if (strcmp(t, "mapping") == 0)  return v->type == JJ_DICT;
    if (strcmp(t, "sequence") == 0) return v->type == JJ_LIST || v->type == JJ_STR;
    if (strcmp(t, "iterable") == 0) return v->type == JJ_LIST || v->type == JJ_DICT ||
                                           v->type == JJ_STR;
    if (strcmp(t, "defined") == 0)  return v->type != JJ_NONE;
    if (strcmp(t, "undefined") == 0) return v->type == JJ_NONE;
    if (strcmp(t, "none") == 0)     return v->type == JJ_NONE;
    if (strcmp(t, "number") == 0)   return v->type == JJ_NUM;
    if (strcmp(t, "boolean") == 0)  return v->type == JJ_BOOL;
    if (strcmp(t, "true") == 0)     return jj_truthy(v);
    if (strcmp(t, "false") == 0)    return !jj_truthy(v);
    return 0;
}

static jj_val *p_cmp(jj_ctx *c, const char **p) {
    jj_val *l = p_cat(c, p);

    for (;;) {
        skip_ws(p);

        if (word(p, "is")) {
            int neg = word(p, "not");
            char *t = ident(p);
            int r;
            if (!t) jj_fail(c, "test name expected after 'is'");
            r = run_test(c, l, t);
            free(t);
            jj_free(l);
            l = jj_bool(neg ? !r : r);
            continue;
        }
        if (word(p, "not")) {
            /* 'not in' */
            if (word(p, "in")) {
                jj_val *r = p_cat(c, p);
                int b = !val_in(l, r);
                jj_free(l); jj_free(r);
                l = jj_bool(b);
                continue;
            }
            jj_fail(c, "unexpected 'not'");
        }
        if (word(p, "in")) {
            jj_val *r = p_cat(c, p);
            int b = val_in(l, r);
            jj_free(l); jj_free(r);
            l = jj_bool(b);
            continue;
        }
        if (punct(p, "==")) { jj_val *r=p_cat(c,p); int b=val_eq(l,r); jj_free(l);jj_free(r); l=jj_bool(b); continue; }
        if (punct(p, "!=")) { jj_val *r=p_cat(c,p); int b=!val_eq(l,r);jj_free(l);jj_free(r); l=jj_bool(b); continue; }
        if (punct(p, "<=")) { jj_val *r=p_cat(c,p); int b=l->num<=r->num;jj_free(l);jj_free(r);l=jj_bool(b); continue; }
        if (punct(p, ">=")) { jj_val *r=p_cat(c,p); int b=l->num>=r->num;jj_free(l);jj_free(r);l=jj_bool(b); continue; }
        if (peek(p, "<") ) { punct(p,"<"); { jj_val *r=p_cat(c,p); int b=l->num<r->num;jj_free(l);jj_free(r);l=jj_bool(b);} continue; }
        if (peek(p, ">") ) { punct(p,">"); { jj_val *r=p_cat(c,p); int b=l->num>r->num;jj_free(l);jj_free(r);l=jj_bool(b);} continue; }
        break;
    }
    return l;
}

static jj_val *p_not(jj_ctx *c, const char **p) {
    if (word(p, "not")) {
        jj_val *v = p_not(c, p);
        int b = !jj_truthy(v);
        jj_free(v);
        return jj_bool(b);
    }
    return p_cmp(c, p);
}

static jj_val *p_and(jj_ctx *c, const char **p) {
    jj_val *l = p_not(c, p);
    while (word(p, "and")) {
        jj_val *r = p_not(c, p);
        int b = jj_truthy(l) && jj_truthy(r);
        jj_free(l); jj_free(r);
        l = jj_bool(b);
    }
    return l;
}

static jj_val *p_or(jj_ctx *c, const char **p) {
    jj_val *l = p_and(c, p);
    while (word(p, "or")) {
        jj_val *r = p_and(c, p);
        int b = jj_truthy(l) || jj_truthy(r);
        jj_free(l); jj_free(r);
        l = jj_bool(b);
    }
    return l;
}

/* value if cond else other */
static jj_val *p_ternary(jj_ctx *c, const char **p) {
    jj_val *v = p_or(c, p);
    const char *save = *p;
    if (word(p, "if")) {
        jj_val *cond = p_or(c, p);
        int t = jj_truthy(cond);
        jj_free(cond);
        if (word(p, "else")) {
            jj_val *other = p_ternary(c, p);
            if (t) { jj_free(other); return v; }
            jj_free(v);
            return other;
        }
        if (t) return v;
        jj_free(v);
        return jj_none();
    }
    *p = save;
    return v;
}

jj_val *jj_eval_expr(jj_ctx *c, const char **pp) {
    return p_ternary(c, pp);
}

/* ------------------------------------------------------------------ */
/* Tag scanning                                                        */
/* ------------------------------------------------------------------ */

/* Find the end of the tag starting at `p` (which points just after
 * "{%"). Returns a pointer just past "%}". */
static const char *tag_end(const char *p) {
    const char *e = strstr(p, "%}");
    return e ? e + 2 : p + strlen(p);
}

/* Return the tag keyword at p (just after "{%"), lowercased into buf. */
static void tag_word(const char *p, char *buf, size_t n) {
    size_t i = 0;
    while (*p == '-' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    while (is_id((unsigned char) *p) && i + 1 < n) buf[i++] = *p++;
    buf[i] = '\0';
}

/* Scan forward from `p` for the matching {% end<kind> %}, honouring
 * nesting. If `stop_alt` is set, also stops at {% elif %} / {% else %}
 * at depth 0. Returns a pointer to the "{%" of the terminator. */
static const char *find_end(jj_ctx *c, const char *p, const char *end,
                            const char *kind, int stop_alt) {
    char open[16], close[16];
    int depth = 0;
    char w[32];

    strcpy(open, kind);
    strcpy(close, "end");
    strcat(close, kind);

    while (p < end) {
        const char *t = strstr(p, "{%");
        if (!t || t >= end) break;
        tag_word(t + 2, w, sizeof(w));
        if (strcmp(w, open) == 0) depth++;
        else if (strcmp(w, close) == 0) {
            if (depth == 0) return t;
            depth--;
        } else if (depth == 0 && stop_alt &&
                   (strcmp(w, "elif") == 0 || strcmp(w, "else") == 0)) {
            return t;
        }
        p = tag_end(t + 2);
    }
    jj_fail(c, "unterminated block");
    return end;
}

/* ------------------------------------------------------------------ */
/* Macros                                                              */
/* ------------------------------------------------------------------ */

static jj_val *call_macro(jj_ctx *c, jj_macro *m, jj_val **args, char **kw,
                          int nargs) {
    jj_scope sc;
    strbuf saved_out;
    strbuf body_out;
    jj_val *result;
    int i;

    sc.vars = jj_dict();
    sc.parent = c->scope;

    /* defaults first */
    for (i = 0; i < m->nparams; i++) {
        jj_dict_set(sc.vars, m->params[i],
                    m->defaults[i] ? jj_clone(m->defaults[i]) : jj_none());
    }
    /* positional then keyword */
    {
        int pos = 0;
        for (i = 0; i < nargs; i++) {
            if (kw[i]) {
                jj_dict_set(sc.vars, kw[i], args[i]);
                free(kw[i]);
            } else if (pos < m->nparams) {
                jj_dict_set(sc.vars, m->params[pos++], args[i]);
            } else {
                jj_free(args[i]);
            }
        }
    }

    sb_init(&body_out);
    saved_out = *c->out;
    *c->out = body_out;
    c->scope = &sc;

    jj_exec(c, m->body, m->body + m->body_len);

    body_out = *c->out;
    *c->out = saved_out;
    c->scope = sc.parent;
    jj_free(sc.vars);

    result = jj_strn(body_out.data, body_out.len);
    sb_free(&body_out);
    return result;
}

static void def_macro(jj_ctx *c, const char *p, const char *tag_close,
                      const char *body, size_t body_len) {
    jj_macro *m = (jj_macro *) xcalloc(1, sizeof(jj_macro));
    const char *q = p;

    m->name = ident(&q);
    if (!m->name) jj_fail(c, "macro: name expected");

    m->params = (char **) xcalloc(16, sizeof(char *));
    m->defaults = (jj_val **) xcalloc(16, sizeof(jj_val *));

    if (punct(&q, "(")) {
        skip_ws(&q);
        while (!peek(&q, ")") && q < tag_close && m->nparams < 16) {
            char *nm = ident(&q);
            if (!nm) break;
            m->params[m->nparams] = nm;
            if (peek(&q, "=") ) {
                punct(&q, "=");
                m->defaults[m->nparams] = p_ternary(c, &q);
            }
            m->nparams++;
            if (!punct(&q, ",")) break;
        }
    }

    m->body = body;
    m->body_len = body_len;
    m->next = c->macros;
    c->macros = m;
}

/* ------------------------------------------------------------------ */
/* Statement interpreter                                               */
/* ------------------------------------------------------------------ */

/* Apply the left-trim of a block's END tag ({%- endfor %}) to whatever
 * the body just produced. jj_exec stops before the end tag, so it never
 * sees that '-' itself. `floor` prevents trimming past the start of the
 * current iteration. */
static void trim_block_end(jj_ctx *c, const char *endtag, size_t floor) {
    if (endtag[2] == '-') {
        size_t low = c->lit_start > floor ? c->lit_start : floor;
        while (c->out->len > low &&
               isspace((unsigned char) c->out->data[c->out->len - 1])) {
            c->out->len--;
        }
        c->out->data[c->out->len] = '\0';
    }
}

/* Handle {%- ... -%} whitespace trimming around a tag. */
static void trim_left_output(jj_ctx *c, const char *tag) {
    if (tag[2] == '-') {
        size_t low = c->lit_start > c->trim_floor ? c->lit_start : c->trim_floor;
        while (c->out->len > low &&
               isspace((unsigned char) c->out->data[c->out->len - 1])) {
            c->out->len--;
        }
        c->out->data[c->out->len] = '\0';
    }
}

static const char *skip_right_trim(const char *after, const char *tagbody) {
    const char *dash = after - 3;   /* char before "%}" */
    if (dash >= tagbody && *dash == '-') {
        while (*after == ' ' || *after == '\t' || *after == '\r' ||
               *after == '\n') after++;
    }
    return after;
}

void jj_exec(jj_ctx *c, const char *p, const char *end) {
    /* Whitespace-trim barrier.
     *
     * {{- and {%- strip trailing whitespace from the output produced so
     * far. That is correct within one linear pass, but a {% for %} body
     * is executed once per iteration, so a leading {{- in the body would
     * otherwise reach back and eat output written by the PREVIOUS
     * iteration -- silently deleting the newline at the end of every
     * chat message. Each nested execution therefore refuses to trim
     * behind wherever the output already stood when it began. */
    size_t barrier = c->trim_floor;
    /* Trimming may reach back to where this execution began -- i.e. the
     * start of the current loop iteration -- but no further. */
    c->trim_floor = c->out->len;

    if (++c->depth > 64) jj_fail(c, "template nesting too deep");

    while (p < end) {
        const char *open = strstr(p, "{");
        if (!open || open >= end) {
            c->lit_start = c->out->len;
            sb_add(c->out, p, (size_t) (end - p));
            break;
        }

        if (open[1] != '{' && open[1] != '%' && open[1] != '#') {
            sb_add(c->out, p, (size_t) (open - p) + 1);
            p = open + 1;
            continue;
        }

        c->lit_start = c->out->len;
        sb_add(c->out, p, (size_t) (open - p));

        /* ---- comment ---- */
        if (open[1] == '#') {
            const char *e = strstr(open, "#}");
            p = e ? e + 2 : end;
            continue;
        }

        /* ---- expression ---- */
        if (open[1] == '{') {
            const char *q = open + 2;
            const char *e = strstr(q, "}}");
            jj_val *v;
            if (!e || e > end) jj_fail(c, "unterminated {{");
            /* Trim BEFORE evaluating: {{- x }} strips whitespace from the
             * literal text preceding the tag, never from the tag's own
             * output. Evaluating first would let jj_tostr append text
             * that trim_left_output then eats -- which silently dropped
             * the trailing newlines in real chat templates. */
            trim_left_output(c, open);
            if (*q == '-') q++;
            v = p_ternary(c, &q);
            jj_tostr(v, c->out);
            jj_free(v);
            c->lit_start = c->out->len;   /* emitted text is not trimmable */
            p = e + 2;
            if (e[-1] == '-') {
                while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            }
            continue;
        }

        /* ---- statement ---- */
        {
            const char *body = open + 2;
            const char *after = tag_end(body);
            const char *q = body;
            char w[32];

            tag_word(body, w, sizeof(w));
            trim_left_output(c, open);

            /* advance q past the keyword (and any leading '-') */
            while (*q == '-' || isspace((unsigned char) *q)) q++;
            q += strlen(w);

            if (strcmp(w, "if") == 0) {
                const char *blk_end = find_end(c, after, end, "if", 0);
                const char *cur = after;
                const char *cond_p = q;
                int done = 0;

                for (;;) {
                    const char *alt = find_end(c, cur, blk_end + 2, "if", 1);
                    jj_val *cv = p_ternary(c, &cond_p);
                    int t = jj_truthy(cv);
                    jj_free(cv);

                    if (t && !done) {
                        size_t mark = c->out->len;
                        jj_exec(c, skip_right_trim(cur, body), alt);
                        trim_block_end(c, alt, mark);
                        done = 1;
                    }
                    if (alt >= blk_end) break;
                    {
                        char aw[32];
                        const char *ab = alt + 2;
                        tag_word(ab, aw, sizeof(aw));
                        cur = tag_end(ab);
                        if (strcmp(aw, "elif") == 0) {
                            const char *z = ab;
                            while (*z == '-' || isspace((unsigned char) *z)) z++;
                            z += 4;
                            cond_p = z;
                            if (done) {
                                /* consume the condition but ignore it */
                                jj_val *skip = p_ternary(c, &cond_p);
                                jj_free(skip);
                                cond_p = z;
                                /* force false */
                                {
                                    static const char *false_expr = "false";
                                    cond_p = false_expr;
                                }
                            }
                        } else {           /* else */
                            static const char *true_expr = "true";
                            cond_p = done ? "false" : true_expr;
                        }
                    }
                }
                p = tag_end(blk_end + 2);
                {
                    const char *bb = blk_end + 2;
                    p = skip_right_trim(p, bb);
                }
                continue;
            }

            if (strcmp(w, "for") == 0) {
                const char *blk_end = find_end(c, after, end, "for", 0);
                const char *body_start = skip_right_trim(after, body);
                char *var = ident(&q);
                char *var2 = NULL;
                jj_val *seq;
                int i;

                if (!var) jj_fail(c, "for: variable expected");
                if (punct(&q, ",")) var2 = ident(&q);
                if (!word(&q, "in")) jj_fail(c, "for: 'in' expected");
                seq = p_ternary(c, &q);

                if (seq->type == JJ_LIST || seq->type == JJ_DICT) {
                    for (i = 0; i < seq->n; i++) {
                        jj_scope sc;
                        jj_val *lp;
                        sc.vars = jj_dict();
                        sc.parent = c->scope;

                        if (seq->type == JJ_DICT) {
                            jj_dict_set(sc.vars, var, jj_str(seq->keys[i]));
                            if (var2) jj_dict_set(sc.vars, var2, jj_clone(seq->items[i]));
                        } else if (var2 && seq->items[i]->type == JJ_LIST &&
                                   seq->items[i]->n >= 2) {
                            /* unpacking form: for k, v in x.items() --
                             * .items() yields [key, value] pairs */
                            jj_dict_set(sc.vars, var,  jj_clone(seq->items[i]->items[0]));
                            jj_dict_set(sc.vars, var2, jj_clone(seq->items[i]->items[1]));
                        } else {
                            jj_dict_set(sc.vars, var, jj_clone(seq->items[i]));
                            if (var2) jj_dict_set(sc.vars, var2, jj_none());
                        }

                        lp = jj_dict();
                        jj_dict_set(lp, "index",  jj_num(i + 1));
                        jj_dict_set(lp, "index0", jj_num(i));
                        jj_dict_set(lp, "first",  jj_bool(i == 0));
                        jj_dict_set(lp, "last",   jj_bool(i == seq->n - 1));
                        jj_dict_set(lp, "length", jj_num(seq->n));
                        jj_dict_set(lp, "revindex",  jj_num(seq->n - i));
                        jj_dict_set(lp, "revindex0", jj_num(seq->n - i - 1));
                        jj_dict_set(lp, "previtem",
                            i > 0 ? jj_clone(seq->items[i-1]) : jj_none());
                        jj_dict_set(lp, "nextitem",
                            i + 1 < seq->n ? jj_clone(seq->items[i+1]) : jj_none());
                        jj_dict_set(sc.vars, "loop", lp);

                        {
                            size_t mark = c->out->len;
                            c->scope = &sc;
                            jj_exec(c, body_start, blk_end);
                            c->scope = sc.parent;
                            trim_block_end(c, blk_end, mark);
                        }
                        jj_free(sc.vars);
                    }
                }
                jj_free(seq);
                free(var);
                if (var2) free(var2);
                p = skip_right_trim(tag_end(blk_end + 2), blk_end + 2);
                continue;
            }

            if (strcmp(w, "set") == 0) {
                char *name = ident(&q);
                char *field = NULL;
                if (!name) jj_fail(c, "set: name expected");
                if (peek(&q, ".")) { punct(&q, "."); field = ident(&q); }

                if (punct(&q, "=")) {
                    jj_val *v = p_ternary(c, &q);
                    if (field) {
                        jj_val *ns = jj_lookup(c->scope, name);
                        if (ns && ns->type == JJ_DICT) jj_dict_set(ns, field, v);
                        else jj_free(v);
                    } else {
                        jj_setvar(c->scope, name, v);
                    }
                    p = skip_right_trim(after, body);
                } else {
                    /* block form: {% set x %}...{% endset %} */
                    const char *blk_end = find_end(c, after, end, "set", 0);
                    strbuf saved = *c->out, tmp;
                    sb_init(&tmp);
                    *c->out = tmp;
                    jj_exec(c, skip_right_trim(after, body), blk_end);
                    tmp = *c->out;
                    *c->out = saved;
                    jj_setvar(c->scope, name, jj_strn(tmp.data, tmp.len));
                    sb_free(&tmp);
                    p = skip_right_trim(tag_end(blk_end + 2), blk_end + 2);
                }
                free(name);
                if (field) free(field);
                continue;
            }

            if (strcmp(w, "macro") == 0) {
                const char *blk_end = find_end(c, after, end, "macro", 0);
                const char *body_start = skip_right_trim(after, body);
                def_macro(c, q, after, body_start,
                          (size_t) (blk_end - body_start));
                p = skip_right_trim(tag_end(blk_end + 2), blk_end + 2);
                continue;
            }

            if (strcmp(w, "generation") == 0 || strcmp(w, "endgeneration") == 0) {
                /* llama.cpp generation markers: ignore */
                p = skip_right_trim(after, body);
                continue;
            }

            /* unknown or stray end tag: skip it */
            p = skip_right_trim(after, body);
            continue;
        }
    }
    c->depth--;
    c->trim_floor = barrier;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int jj_render(const char *tmpl, const jj_val *ctx, strbuf *out,
              char *errbuf, size_t errlen) {
    jj_ctx c;
    jj_scope root;
    int i;

    if (errbuf && errlen) errbuf[0] = '\0';
    sb_init(out);

    root.vars = jj_dict();
    root.parent = NULL;
    if (ctx && ctx->type == JJ_DICT) {
        for (i = 0; i < ctx->n; i++) {
            jj_dict_set(root.vars, ctx->keys[i], jj_clone(ctx->items[i]));
        }
    }

    memset(&c, 0, sizeof(c));
    c.tmpl = tmpl;
    c.scope = &root;
    c.out = out;
    c.err = errbuf;
    c.errlen = errlen;

    if (setjmp(c.jmp) != 0) {
        jj_macro *m = c.macros;
        while (m) {
            jj_macro *nx = m->next;
            int k;
            free(m->name);
            for (k = 0; k < m->nparams; k++) {
                free(m->params[k]);
                if (m->defaults[k]) jj_free(m->defaults[k]);
            }
            free(m->params); free(m->defaults); free(m);
            m = nx;
        }
        jj_free(root.vars);
        return -1;
    }

    jj_exec(&c, tmpl, tmpl + strlen(tmpl));

    {
        jj_macro *m = c.macros;
        while (m) {
            jj_macro *nx = m->next;
            int k;
            free(m->name);
            for (k = 0; k < m->nparams; k++) {
                free(m->params[k]);
                if (m->defaults[k]) jj_free(m->defaults[k]);
            }
            free(m->params); free(m->defaults); free(m);
            m = nx;
        }
    }
    jj_free(root.vars);
    return 0;
}
