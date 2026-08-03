/* jinja.h -- a small Jinja2 subset interpreter, enough to render the
 * chat templates embedded in GGUF files.
 *
 * This is not a general Jinja implementation. It supports exactly the
 * constructs that appear in real chat templates:
 *
 *   {{ expr }}                       output, with - whitespace control
 *   {% if %} {% elif %} {% else %} {% endif %}
 *   {% for x in seq %} ... {% endfor %}   with loop.index0/first/last/
 *                                          previtem/nextitem
 *   {% set name = expr %}
 *   {% macro name(args) %} ... {% endmacro %}   and calls
 *   namespace(a=1)  and  ns.a = ...
 *   filters: trim, tojson, string, length, list, safe
 *   tests:   is string / mapping / iterable / defined / none / not ...
 *   operators: and or not == != < > <= >= + ~ in, ternary if/else
 *   methods: .split(s) .startswith(s) .endswith(s) .lstrip(c) .rstrip(c)
 *   slicing: seq[::-1], seq[n], obj.attr, obj['key']
 *   raise_exception('msg')
 *
 * Values are dynamically typed (see jj_val). The caller builds the
 * input context out of jj_val objects -- typically `messages`, `tools`,
 * `add_generation_prompt` and `enable_thinking`.
 *
 * ANSI C (C89).
 */

#ifndef INFER_JINJA_H
#define INFER_JINJA_H

#include "infer.h"

#define JJ_NONE  0
#define JJ_BOOL  1
#define JJ_NUM   2
#define JJ_STR   3
#define JJ_LIST  4
#define JJ_DICT  5

typedef struct jj_val jj_val;

struct jj_val {
    int      type;
    double   num;        /* JJ_NUM, and 0/1 for JJ_BOOL          */
    char    *str;        /* JJ_STR (owned)                        */
    jj_val **items;      /* JJ_LIST / JJ_DICT values (owned)      */
    char   **keys;       /* JJ_DICT keys (owned)                  */
    int      n;
    int      cap;
};

/* --- constructors; all values are heap-allocated and owned by the
 *     caller unless handed to jj_list_add / jj_dict_set --- */
jj_val *jj_none(void);
jj_val *jj_bool(int b);
jj_val *jj_num(double d);
jj_val *jj_str(const char *s);
jj_val *jj_strn(const char *s, size_t n);
jj_val *jj_list(void);
jj_val *jj_dict(void);
void    jj_list_add(jj_val *l, jj_val *v);          /* takes ownership */
void    jj_dict_set(jj_val *d, const char *k, jj_val *v); /* takes ownership */
jj_val *jj_dict_get(const jj_val *d, const char *k);
jj_val *jj_clone(const jj_val *v);           /* deep copy */
void    jj_free(jj_val *v);

/* Render `tmpl` with the given root context (a JJ_DICT).
 * On success returns 0 and fills `out` (which the caller must sb_free).
 * On error returns -1 and writes a message into errbuf. */
int jj_render(const char *tmpl, const jj_val *ctx, strbuf *out,
              char *errbuf, size_t errlen);

#endif
