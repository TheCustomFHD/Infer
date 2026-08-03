/* jinja_priv.h -- shared internals between jinja.c (values) and
 * jinja_eval.c (parser / interpreter). Not a public interface.
 * ANSI C (C89). */

#ifndef INFER_JINJA_PRIV_H
#define INFER_JINJA_PRIV_H

#include "jinja.h"

#include <setjmp.h>

/* Variable scope: a chain of frames, innermost first. */
typedef struct jj_scope {
    jj_val          *vars;    /* JJ_DICT, owned */
    struct jj_scope *parent;
} jj_scope;

/* A user-defined {% macro %}. */
typedef struct jj_macro {
    char             *name;
    char            **params;     /* parameter names        */
    jj_val          **defaults;   /* default values or NULL */
    int               nparams;
    const char       *body;       /* points into the template */
    size_t            body_len;
    struct jj_macro  *next;
} jj_macro;

typedef struct {
    const char *tmpl;
    jj_scope   *scope;
    jj_macro   *macros;
    strbuf     *out;
    char       *err;
    size_t      errlen;
    jmp_buf     jmp;          /* longjmp target for raise_exception */
    int         depth;
    size_t      trim_floor;
    size_t      lit_start;    /* start of the current literal text run */   /* see trim_left_output() in jinja_eval.c */
} jj_ctx;

/* from jinja.c */
int     jj_truthy(const jj_val *v);
void    jj_tostr(const jj_val *v, strbuf *b);
void    jj_tojson(const jj_val *v, strbuf *b);

/* from jinja_eval.c */
void    jj_fail(jj_ctx *c, const char *msg);
jj_val *jj_lookup(jj_scope *s, const char *name);
void    jj_setvar(jj_scope *s, const char *name, jj_val *v);
jj_val *jj_eval_expr(jj_ctx *c, const char **pp);
void    jj_exec(jj_ctx *c, const char *p, const char *end);

#endif
