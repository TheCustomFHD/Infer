/* prof.c -- profiling and throughput logging.
 *
 * See prof.h for the design. The stage-profiling half of this file is
 * compiled only under -DINFER_PROFILE; the throughput half is always
 * present because it costs two clock reads per request.
 *
 * ANSI C (C89).
 */

#include "infer.h"
#include "prof.h"
#include "sys.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Log sink                                                            */
/* ------------------------------------------------------------------ */

static FILE *log_fp = NULL;
static int   log_owned = 0;

int prof_perf_enabled = 0;
int prof_stages_enabled = 0;

int prof_open_log(const char *path) {
    prof_close_log();

    if (!path || !path[0] || strcmp(path, "-") == 0) {
        log_fp = stderr;
        log_owned = 0;
        return 0;
    }

    log_fp = fopen(path, "a");
    if (!log_fp) {
        log_fp = stderr;
        log_owned = 0;
        return -1;
    }
    log_owned = 1;
    return 0;
}

void prof_close_log(void) {
    if (log_fp && log_owned) fclose(log_fp);
    log_fp = NULL;
    log_owned = 0;
}

FILE *prof_log_file(void) {
    return log_fp ? log_fp : stderr;
}

void prof_logf(const char *fmt, ...) {
    FILE *f = prof_log_file();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);   /* a slow machine may be killed mid-run; do not buffer */
}

void prof_log_banner(const char *model_path, const char *backend,
                     const char *cpu_vendor, int mmx, int tdnow, int cmov,
                     int n_ctx) {
    time_t now = time(NULL);

    prof_logf("\n");
    prof_logf("========================================================\n");
    prof_logf("infer %s -- performance log\n", INFER_VERSION);
    prof_logf("started      : %s", ctime(&now));   /* ctime ends in \n */
    prof_logf("model        : %s\n", model_path ? model_path : "?");
    prof_logf("backend      : %s\n", backend ? backend : "?");
    prof_logf("cpu          : %s  features:%s%s%s\n",
              cpu_vendor ? cpu_vendor : "?",
              mmx   ? " mmx"   : "",
              tdnow ? " 3dnow" : "",
              cmov  ? " cmov"  : "");
    prof_logf("context      : %d tokens\n", n_ctx);
    prof_logf("stage profile: %s\n",
              PROF_ENABLED ? "compiled in (-DINFER_PROFILE)"
                           : "not compiled in");
    prof_logf("pointer size : %d bits\n", (int) (sizeof(void *) * 8));
    prof_logf("========================================================\n");
}

/* ------------------------------------------------------------------ */
/* Throughput logging                                                  */
/* ------------------------------------------------------------------ */

void perf_begin(perf_run *r) {
    memset(r, 0, sizeof(*r));
    if (!prof_perf_enabled) return;
    r->t_start = sys_time_sec();
}

void perf_mark_prompt_done(perf_run *r, int n_prompt) {
    if (!prof_perf_enabled) return;
    r->t_prompt_end = sys_time_sec();
    r->n_prompt = n_prompt;
}

void perf_mark_first_token(perf_run *r) {
    if (!prof_perf_enabled || r->have_first) return;
    r->t_first_tok = sys_time_sec();
    r->have_first = 1;
}

void perf_end(perf_run *r, int n_gen) {
    if (!prof_perf_enabled) return;
    r->t_end = sys_time_sec();
    r->n_gen = n_gen;
}

/* Format a duration compactly: milliseconds, seconds or minutes. */
static void fmt_dur(double s, char *out) {
    if (s < 1.0)        sprintf(out, "%.0f ms", s * 1000.0);
    else if (s < 120.0) sprintf(out, "%.2f s", s);
    else                sprintf(out, "%.1f min", s / 60.0);
}

void perf_report(const perf_run *r, const char *tag) {
    double t_prompt, t_gen, t_total, ttft;
    char b1[32], b2[32], b3[32], b4[32];

    if (!prof_perf_enabled) return;

    t_prompt = r->t_prompt_end - r->t_start;
    t_gen    = r->t_end - r->t_prompt_end;
    t_total  = r->t_end - r->t_start;
    ttft     = r->have_first ? (r->t_first_tok - r->t_start) : 0.0;

    fmt_dur(t_prompt, b1);
    fmt_dur(t_gen,    b2);
    fmt_dur(t_total,  b3);
    fmt_dur(ttft,     b4);

    prof_logf("\n--- perf: %s ---\n", tag ? tag : "request");
    prof_logf("  prompt      : %5d tok in %-10s", r->n_prompt, b1);
    if (t_prompt > 0.0 && r->n_prompt > 0) {
        prof_logf("(%.3f tok/s, %.2f s/tok)",
                  r->n_prompt / t_prompt, t_prompt / r->n_prompt);
    }
    prof_logf("\n");

    prof_logf("  generation  : %5d tok in %-10s", r->n_gen, b2);
    if (t_gen > 0.0 && r->n_gen > 0) {
        prof_logf("(%.3f tok/s, %.2f s/tok)",
                  r->n_gen / t_gen, t_gen / r->n_gen);
    }
    prof_logf("\n");

    prof_logf("  TTFT        : %s\n", b4);
    prof_logf("  total       : %s\n", b3);
}

void perf_total_init(perf_total *t) {
    memset(t, 0, sizeof(*t));
}

void perf_total_add(perf_total *t, const perf_run *r) {
    if (!prof_perf_enabled) return;
    t->t_prompt += r->t_prompt_end - r->t_start;
    t->t_gen    += r->t_end - r->t_prompt_end;
    t->n_prompt += r->n_prompt;
    t->n_gen    += r->n_gen;
    t->n_turns  += 1;
}

void perf_total_report(const perf_total *t, const char *tag) {
    char b1[32], b2[32], b3[32];

    if (!prof_perf_enabled || t->n_turns == 0) return;

    fmt_dur(t->t_prompt, b1);
    fmt_dur(t->t_gen, b2);
    fmt_dur(t->t_prompt + t->t_gen, b3);

    prof_logf("\n--- perf: %s (%d turn%s) ---\n",
              tag ? tag : "session", t->n_turns,
              t->n_turns == 1 ? "" : "s");

    prof_logf("  prompt      : %5d tok in %-10s", t->n_prompt, b1);
    if (t->t_prompt > 0.0 && t->n_prompt > 0) {
        prof_logf("(%.3f tok/s, %.2f s/tok)",
                  t->n_prompt / t->t_prompt, t->t_prompt / t->n_prompt);
    }
    prof_logf("\n");

    prof_logf("  generation  : %5d tok in %-10s", t->n_gen, b2);
    if (t->t_gen > 0.0 && t->n_gen > 0) {
        prof_logf("(%.3f tok/s, %.2f s/tok)",
                  t->n_gen / t->t_gen, t->t_gen / t->n_gen);
    }
    prof_logf("\n");

    prof_logf("  total       : %s\n", b3);
}

/* ------------------------------------------------------------------ */
/* Stage profiling                                                     */
/* ------------------------------------------------------------------ */

#ifdef INFER_PROFILE

typedef struct {
    double total;      /* seconds accumulated */
    double work;       /* work units (MACs)   */
    long   calls;
} pstage;

static pstage stages[PF_NSTAGES];

double prof_now(void) {
    return sys_time_sec();
}

int prof_is_enabled(void) {
    return 1;
}

void prof_add(int stage, double elapsed, double work) {
    if (stage < 0 || stage >= PF_NSTAGES) return;
    stages[stage].total += elapsed;
    stages[stage].work  += work;
    stages[stage].calls += 1;
}

void prof_reset(void) {
    memset(stages, 0, sizeof(stages));
}

static const char *prof_stage_name(int s) {
    switch (s) {
        case PF_TOTAL:        return "TOTAL forward pass";
        case PF_EMBED:        return "  embedding lookup";
        case PF_ATTN:         return "  attention layers (6)";
        case PF_ATTN_QKV:     return "    qkv projections";
        case PF_ATTN_SCORE:   return "    scores+softmax+AV";
        case PF_ATTN_OUT:     return "    output projection";
        case PF_DELTANET:     return "  delta-net layers (18)";
        case PF_DN_PROJ:      return "    input projections";
        case PF_DN_CONV:      return "    causal conv + SiLU";
        case PF_DN_RECUR:     return "    state recurrence";
        case PF_DN_OUT:       return "    output projection";
        case PF_FFN:          return "  feed-forward (24)";
        case PF_NORM:         return "  rms norms";
        case PF_LMHEAD:       return "  lm head";
        case PF_MATVEC_Q4K:   return "matvec Q4_K";
        case PF_MATVEC_Q5K:   return "matvec Q5_K";
        case PF_MATVEC_Q6K:   return "matvec Q6_K";
        case PF_MATVEC_Q80:   return "matvec Q8_0";
        case PF_MATVEC_OTHER: return "matvec other";
        default:              return "?";
    }
}

void prof_report(void) {
    double total = stages[PF_TOTAL].total;
    int i;

    /* Compiled in, but silent unless --log-stages asked for it. */
    if (!prof_stages_enabled) return;
    if (stages[PF_TOTAL].calls == 0) return;

    prof_logf("\n");
    prof_logf("========================================================\n");
    prof_logf("STAGE PROFILE  (%ld forward passes)\n",
              stages[PF_TOTAL].calls);
    prof_logf("--------------------------------------------------------\n");
    prof_logf("%-24s %10s %7s %9s %8s\n",
              "stage", "seconds", "%", "calls", "us/call");
    prof_logf("--------------------------------------------------------\n");

    for (i = 0; i < PF_NSTAGES; i++) {
        double pct;
        if (stages[i].calls == 0) continue;
        if (i == PF_MATVEC_Q4K) {
            prof_logf("--------------------------------------------------------\n");
            prof_logf("by weight format (overlaps the stages above)\n");
            prof_logf("--------------------------------------------------------\n");
        }
        pct = total > 0.0 ? 100.0 * stages[i].total / total : 0.0;
        prof_logf("%-24s %10.3f %6.1f%% %9ld %8.1f\n",
                  prof_stage_name(i),
                  stages[i].total,
                  pct,
                  stages[i].calls,
                  stages[i].calls
                    ? stages[i].total / stages[i].calls * 1e6 : 0.0);
    }

    prof_logf("--------------------------------------------------------\n");

    /* Arithmetic throughput of the matvec kernels, which is where
     * essentially all the work is. */
    {
        double w = 0.0, t = 0.0;
        for (i = PF_MATVEC_Q4K; i <= PF_MATVEC_OTHER; i++) {
            w += stages[i].work;
            t += stages[i].total;
        }
        if (t > 0.0 && w > 0.0) {
            prof_logf("matvec throughput      : %.1f Mflop/s "
                      "(%.0f Mflop in %.2f s)\n",
                      w * 2.0 / t / 1e6, w * 2.0 / 1e6, t);
            prof_logf("weights streamed       : ~%.0f MB per forward pass\n",
                      w / (double) stages[PF_TOTAL].calls * 0.6 / 1048576.0);
        }
        if (total > 0.0) {
            prof_logf("time per forward pass  : %.3f s\n",
                      total / (double) stages[PF_TOTAL].calls);
        }
    }
    prof_logf("========================================================\n");
}

#endif /* INFER_PROFILE */
