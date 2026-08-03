/* prof.h -- optional per-stage profiling and throughput logging.
 *
 * Two independent, separately-enabled features:
 *
 *   1. STAGE PROFILING (compile time: -DINFER_PROFILE)
 *      Accumulates wall-clock time and call counts for each stage of
 *      the forward pass -- embedding lookup, attention, delta-net, FFN,
 *      the LM head, and the individual matvecs inside each. Written as
 *      a report at exit.
 *
 *      This is a COMPILE-TIME option. Without -DINFER_PROFILE every
 *      macro below expands to nothing at all: no timer reads, no
 *      counters, no branches, not one extra instruction in the hot
 *      loop. That matters here -- on a 500 MHz Geode a gettimeofday()
 *      per matvec would itself distort what we are trying to measure.
 *
 *   2. THROUGHPUT LOGGING (always compiled, enabled at run time)
 *      Time-to-first-token, tokens/second for prompt ingestion and for
 *      generation, per request. Costs two clock reads per request, so
 *      it is always available; turn it on with --log-perf.
 *
 * Both are OPT-IN. A -DINFER_PROFILE build compiles the stage timers in
 * but still prints nothing until --log-stages is given: building for
 * measurement and asking for measurements are separate decisions.
 *
 * Both write to stderr by default, or to a file given with --log-file.
 *
 * ANSI C (C89).
 */

#ifndef INFER_PROF_H
#define INFER_PROF_H

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Output sink (shared by both features)                               */
/* ------------------------------------------------------------------ */

/* Open the log file. Pass NULL or "-" for stderr. Returns 0 on success. */
int   prof_open_log(const char *path);
void  prof_close_log(void);
FILE *prof_log_file(void);

/* Write a line to the log (adds no newline; caller supplies it). */
void  prof_logf(const char *fmt, ...);

/* Emit a header describing the machine and build, once, at startup. */
void  prof_log_banner(const char *model_path, const char *backend,
                      const char *cpu_vendor, int mmx, int tdnow, int cmov,
                      int n_ctx);

/* ------------------------------------------------------------------ */
/* Throughput logging (run-time switch, always compiled in)            */
/* ------------------------------------------------------------------ */

extern int prof_perf_enabled;      /* set by --log-perf   */
extern int prof_stages_enabled;    /* set by --log-stages */

typedef struct {
    double t_start;        /* request accepted                      */
    double t_prompt_end;   /* last prompt token consumed            */
    double t_first_tok;    /* first output token produced (TTFT)    */
    double t_end;          /* generation finished                   */
    int    n_prompt;
    int    n_gen;
    int    have_first;
} perf_run;

void perf_begin(perf_run *r);
void perf_mark_prompt_done(perf_run *r, int n_prompt);
void perf_mark_first_token(perf_run *r);
void perf_end(perf_run *r, int n_gen);
/* Write the one-line-per-phase summary for a finished run. */
void perf_report(const perf_run *r, const char *tag);

/* Running totals across several runs, for a chat session. */
typedef struct {
    double t_prompt;
    double t_gen;
    int    n_prompt;
    int    n_gen;
    int    n_turns;
} perf_total;

void perf_total_init(perf_total *t);
void perf_total_add(perf_total *t, const perf_run *r);
/* Session summary: totals plus the averaged rates. */
void perf_total_report(const perf_total *t, const char *tag);

/* ------------------------------------------------------------------ */
/* Stage profiling (compile-time switch)                               */
/* ------------------------------------------------------------------ */

/* Stage identifiers. Keep in sync with prof_stage_name() in prof.c. */
enum {
    PF_TOTAL = 0,      /* whole forward pass                        */
    PF_EMBED,          /* token embedding row dequantisation        */
    PF_ATTN,           /* full-attention layers, all of it          */
    PF_ATTN_QKV,       /*   ... Q/K/V projections                   */
    PF_ATTN_SCORE,     /*   ... scores + softmax + weighted sum     */
    PF_ATTN_OUT,       /*   ... output projection                   */
    PF_DELTANET,       /* gated delta-net layers, all of it         */
    PF_DN_PROJ,        /*   ... qkv/gate/beta/alpha projections     */
    PF_DN_CONV,        /*   ... causal depthwise conv + SiLU        */
    PF_DN_RECUR,       /*   ... the S-matrix recurrence             */
    PF_DN_OUT,         /*   ... output projection                   */
    PF_FFN,            /* SwiGLU feed-forward                       */
    PF_NORM,           /* RMS norms in the block loop               */
    PF_LMHEAD,         /* final norm + vocabulary projection        */
    PF_MATVEC_Q4K,     /* matvec by weight type (overlaps the above)*/
    PF_MATVEC_Q5K,
    PF_MATVEC_Q6K,
    PF_MATVEC_Q80,
    PF_MATVEC_OTHER,
    PF_NSTAGES
};

#ifdef INFER_PROFILE

void   prof_add(int stage, double elapsed, double work);
void   prof_reset(void);
void   prof_report(void);
double prof_now(void);
int    prof_is_enabled(void);

/* Usage:
 *     PROF_DECL(t);
 *     PROF_START(t);
 *     ... work ...
 *     PROF_STOP(t, PF_FFN, flops);
 *
 * `work` is an arbitrary work unit (we pass MACs) used to derive a rate
 * in the report; pass 0.0 if not meaningful. */
#define PROF_DECL(v)            double v
#define PROF_START(v)           ((v) = prof_now())
#define PROF_STOP(v, stage, w)  prof_add((stage), prof_now() - (v), (double)(w))
#define PROF_ENABLED            1

#else

/* Disabled: every macro vanishes. The (void)0 keeps the trailing
 * semicolon at call sites legal in C89. */
#define PROF_DECL(v)            /* nothing */
#define PROF_START(v)           ((void)0)
#define PROF_STOP(v, stage, w)  ((void)0)
#define PROF_ENABLED            0

#define prof_reset()            ((void)0)
#define prof_report()           ((void)0)
#define prof_is_enabled()       0

#endif /* INFER_PROFILE */

#endif /* INFER_PROF_H */
