/* json.h -- minimal read-only JSON parser used by the OpenAI endpoint.
 * ANSI C (C89). */

#ifndef INFER_JSON_H
#define INFER_JSON_H

#include <stddef.h>

#define JS_NULL   0
#define JS_BOOL   1
#define JS_NUM    2
#define JS_STR    3
#define JS_ARR    4
#define JS_OBJ    5

typedef struct js_val js_val;

struct js_val {
    int      type;
    double   num;        /* JS_NUM, and 0/1 for JS_BOOL */
    char    *str;        /* JS_STR: decoded, NUL-terminated (owned) */
    js_val **items;      /* JS_ARR / JS_OBJ values (owned) */
    char   **keys;       /* JS_OBJ keys (owned) */
    int      n;          /* item count */
};

/* Parse a whole document. Returns NULL on syntax error. */
js_val *js_parse(const char *text);
void    js_free(js_val *v);

/* Object member lookup; NULL if absent or not an object. */
js_val     *js_get(const js_val *o, const char *key);
const char *js_str(const js_val *o, const char *key, const char *dflt);
double      js_num(const js_val *o, const char *key, double dflt);
int         js_bool(const js_val *o, const char *key, int dflt);

#endif
