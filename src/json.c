/* json.c -- minimal recursive-descent JSON parser.
 *
 * Enough of RFC 8259 to read OpenAI chat/completion request bodies:
 * objects, arrays, strings (with \u escapes re-encoded to UTF-8),
 * numbers, true/false/null.
 *
 * ANSI C (C89).
 */

#include "json.h"
#include "infer.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *p;
    int         depth;
    int         failed;
} jp;

#define JS_MAX_DEPTH 64

static js_val *parse_value(jp *s);

static void skip_ws(jp *s) {
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') {
        s->p++;
    }
}

static js_val *new_val(int type) {
    js_val *v = (js_val *) xcalloc(1, sizeof(js_val));
    v->type = type;
    return v;
}

/* Encode one codepoint as UTF-8 into buf; returns bytes written. */
static int enc_utf8(unsigned long cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char) cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char) (0xC0 | (cp >> 6));
        buf[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char) (0xE0 | (cp >> 12));
        buf[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char) (0xF0 | (cp >> 18));
    buf[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char) (0x80 | (cp & 0x3F));
    return 4;
}

static int hex4(const char *p, unsigned long *out) {
    int i;
    unsigned long v = 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (unsigned long) (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long) (c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Parse a JSON string literal into a freshly allocated C string. */
static char *parse_string(jp *s) {
    strbuf b;
    char tmp[8];

    if (*s->p != '"') { s->failed = 1; return NULL; }
    s->p++;

    sb_init(&b);
    while (*s->p && *s->p != '"') {
        if (*s->p == '\\') {
            s->p++;
            switch (*s->p) {
                case '"':  sb_add(&b, "\"", 1); s->p++; break;
                case '\\': sb_add(&b, "\\", 1); s->p++; break;
                case '/':  sb_add(&b, "/",  1); s->p++; break;
                case 'b':  sb_add(&b, "\b", 1); s->p++; break;
                case 'f':  sb_add(&b, "\f", 1); s->p++; break;
                case 'n':  sb_add(&b, "\n", 1); s->p++; break;
                case 'r':  sb_add(&b, "\r", 1); s->p++; break;
                case 't':  sb_add(&b, "\t", 1); s->p++; break;
                case 'u': {
                    unsigned long cp;
                    s->p++;
                    if (hex4(s->p, &cp) != 0) { s->failed = 1; goto done; }
                    s->p += 4;
                    /* surrogate pair */
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        s->p[0] == '\\' && s->p[1] == 'u') {
                        unsigned long lo;
                        if (hex4(s->p + 2, &lo) == 0 &&
                            lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            s->p += 6;
                        }
                    }
                    sb_add(&b, tmp, (size_t) enc_utf8(cp, tmp));
                    break;
                }
                default:
                    s->failed = 1;
                    goto done;
            }
        } else {
            sb_add(&b, s->p, 1);
            s->p++;
        }
    }

    if (*s->p != '"') { s->failed = 1; goto done; }
    s->p++;

done:
    if (s->failed) {
        sb_free(&b);
        return NULL;
    }
    return b.data;   /* ownership transferred */
}

static void obj_push(js_val *o, char *key, js_val *val) {
    int n = o->n + 1;
    o->items = (js_val **) xrealloc(o->items, sizeof(js_val *) * (size_t) n);
    if (key) {
        o->keys = (char **) xrealloc(o->keys, sizeof(char *) * (size_t) n);
        o->keys[o->n] = key;
    }
    o->items[o->n] = val;
    o->n = n;
}

static js_val *parse_value(jp *s) {
    js_val *v;

    if (s->failed) return NULL;
    if (++s->depth > JS_MAX_DEPTH) { s->failed = 1; return NULL; }

    skip_ws(s);

    switch (*s->p) {
        case '{': {
            v = new_val(JS_OBJ);
            s->p++;
            skip_ws(s);
            if (*s->p == '}') { s->p++; break; }
            for (;;) {
                char *key;
                js_val *val;
                skip_ws(s);
                key = parse_string(s);
                if (s->failed) { js_free(v); s->depth--; return NULL; }
                skip_ws(s);
                if (*s->p != ':') {
                    s->failed = 1;
                    free(key);
                    js_free(v);
                    s->depth--;
                    return NULL;
                }
                s->p++;
                val = parse_value(s);
                if (s->failed) {
                    free(key);
                    js_free(v);
                    s->depth--;
                    return NULL;
                }
                obj_push(v, key, val);
                skip_ws(s);
                if (*s->p == ',') { s->p++; continue; }
                if (*s->p == '}') { s->p++; break; }
                s->failed = 1;
                js_free(v);
                s->depth--;
                return NULL;
            }
            break;
        }

        case '[': {
            v = new_val(JS_ARR);
            s->p++;
            skip_ws(s);
            if (*s->p == ']') { s->p++; break; }
            for (;;) {
                js_val *val = parse_value(s);
                if (s->failed) { js_free(v); s->depth--; return NULL; }
                obj_push(v, NULL, val);
                skip_ws(s);
                if (*s->p == ',') { s->p++; continue; }
                if (*s->p == ']') { s->p++; break; }
                s->failed = 1;
                js_free(v);
                s->depth--;
                return NULL;
            }
            break;
        }

        case '"':
            v = new_val(JS_STR);
            v->str = parse_string(s);
            if (s->failed) { js_free(v); s->depth--; return NULL; }
            break;

        case 't':
            if (strncmp(s->p, "true", 4) != 0) { s->failed = 1; s->depth--; return NULL; }
            s->p += 4;
            v = new_val(JS_BOOL);
            v->num = 1.0;
            break;

        case 'f':
            if (strncmp(s->p, "false", 5) != 0) { s->failed = 1; s->depth--; return NULL; }
            s->p += 5;
            v = new_val(JS_BOOL);
            v->num = 0.0;
            break;

        case 'n':
            if (strncmp(s->p, "null", 4) != 0) { s->failed = 1; s->depth--; return NULL; }
            s->p += 4;
            v = new_val(JS_NULL);
            break;

        default: {
            char *end;
            double d;
            if (*s->p != '-' && *s->p != '+' && !isdigit((unsigned char) *s->p)) {
                s->failed = 1;
                s->depth--;
                return NULL;
            }
            d = strtod(s->p, &end);
            if (end == s->p) { s->failed = 1; s->depth--; return NULL; }
            s->p = end;
            v = new_val(JS_NUM);
            v->num = d;
            break;
        }
    }

    s->depth--;
    return v;
}

js_val *js_parse(const char *text) {
    jp s;
    js_val *v;

    s.p = text;
    s.depth = 0;
    s.failed = 0;

    v = parse_value(&s);
    if (s.failed) {
        js_free(v);
        return NULL;
    }
    skip_ws(&s);
    return v;
}

void js_free(js_val *v) {
    int i;
    if (!v) return;
    if (v->str) free(v->str);
    for (i = 0; i < v->n; i++) {
        if (v->items) js_free(v->items[i]);
        if (v->keys && v->keys[i]) free(v->keys[i]);
    }
    if (v->items) free(v->items);
    if (v->keys)  free(v->keys);
    free(v);
}

js_val *js_get(const js_val *o, const char *key) {
    int i;
    if (!o || o->type != JS_OBJ || !o->keys) return NULL;
    for (i = 0; i < o->n; i++) {
        if (o->keys[i] && strcmp(o->keys[i], key) == 0) return o->items[i];
    }
    return NULL;
}

const char *js_str(const js_val *o, const char *key, const char *dflt) {
    js_val *v = js_get(o, key);
    if (!v || v->type != JS_STR) return dflt;
    return v->str;
}

double js_num(const js_val *o, const char *key, double dflt) {
    js_val *v = js_get(o, key);
    if (!v || (v->type != JS_NUM && v->type != JS_BOOL)) return dflt;
    return v->num;
}

int js_bool(const js_val *o, const char *key, int dflt) {
    js_val *v = js_get(o, key);
    if (!v) return dflt;
    if (v->type == JS_BOOL || v->type == JS_NUM) return v->num != 0.0;
    return dflt;
}
