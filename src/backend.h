/* backend.h -- selectable matrix-vector kernels.
 *
 * Profiling shows ~95% of all inference time is spent in one operation:
 * multiplying a quantised weight matrix by an activation vector. This
 * header lets that one operation be swapped at compile time or at run
 * time without touching the engine.
 *
 * Backends:
 *
 *   ref   Portable ANSI C, scalar float. Dequantises each weight to
 *         float and multiply-accumulates. Runs on a bare 486 (x87 only,
 *         no SIMD, no CMOV). This is the reference: every other backend
 *         is checked against it.
 *
 *   i8    Portable ANSI C, integer. Quantises the activation vector to
 *         int8 once per matrix, then evaluates each dot product with
 *         integer multiply-accumulate, applying the block scales once
 *         per 32 weights instead of once per weight. Same instruction
 *         set requirements as `ref` -- still 486-clean -- but does far
 *         less work.
 *
 *   mmx   As i8, with the integer inner loop in MMX (PMADDWD, 4 lanes).
 *         Requires MMX: Pentium MMX, K6, Geode GX2/LX and later.
 *         Compiled only when INFER_HAVE_MMX is defined, and used only
 *         if CPUID confirms the CPU has MMX.
 *
 * Selection:
 *   compile time   -DINFER_BACKEND=\"i8\"   sets the default
 *   run time       --backend <name>        overrides it
 *                  --backend list          shows what is available
 * The default is "auto": the fastest backend this CPU supports.
 *
 * ANSI C (C89). The MMX backend uses GNU inline assembly and is the
 * only file in the project that is not strictly conforming; it is
 * optional and excluded from the portable build.
 */

#ifndef INFER_BACKEND_H
#define INFER_BACKEND_H

#include <stdio.h>

typedef void (*bk_matvec_fn)(int type, const unsigned char *w,
                             const float *x, float *y,
                             long ncols, long nrows);

typedef struct {
    const char  *name;
    const char  *desc;
    int        (*available)(void);   /* 1 if usable on this CPU */
    bk_matvec_fn matvec;
} bk_backend;

/* Choose a backend by name, or "auto". Returns 0 on success, -1 if the
 * name is unknown, -2 if it is known but unsupported on this CPU. */
int         bk_select(const char *name);

const char *bk_name(void);
const char *bk_desc(void);

/* Print the backend table, marking availability and the current choice. */
void        bk_print_list(FILE *f);

/* CPU feature probe (CPUID where available). */
int         bk_cpu_has_mmx(void);
int         bk_cpu_has_3dnow(void);
int         bk_cpu_has_cmov(void);
int         bk_cpu_has_sse2(void);
/* AVX2 *and* OS ymm support (CPUID + XGETBV). Windows XP fails this
 * even on silicon that has AVX2 -- see backend.c. */
int         bk_cpu_has_avx2(void);
const char *bk_cpu_vendor(void);

/* The individual kernels, exposed so tests can compare them directly. */
void qmv_ref(int type, const unsigned char *w, const float *x, float *y,
             long ncols, long nrows);
void qmv_i8 (int type, const unsigned char *w, const float *x, float *y,
             long ncols, long nrows);
#ifdef INFER_HAVE_MMX
void qmv_mmx(int type, const unsigned char *w, const float *x, float *y,
             long ncols, long nrows);
#endif

#ifdef INFER_HAVE_AVX2
/* AVX2 kernels built around VPMADDUBSW. See backend_avx2.c, which is
 * the ONLY object compiled with -mavx2; everything else is i486
 * baseline. Declared on INFER_HAVE_AVX2 (not __AVX2__) because the
 * callers are baseline objects. */
void qmv_avx2(int type, const unsigned char *w, const float *x, float *y,
              long ncols, long nrows);
int  bk_avx2_available(void);
#endif

/* AVX2 helpers for code that lives in i486-baseline translation units.
 *
 * backend.c and qwen35.c are compiled at the i486 baseline so one
 * binary runs on a 486 and still accelerates on a modern CPU, which
 * means they cannot contain AVX2 instructions. These are defined in
 * backend_avx2.c (the only -mavx2 object) and each returns 1 if it did
 * the work, 0 if AVX2 is unavailable and the caller must use its own
 * scalar loop. Always declared, so the call sites need no #ifdef. */
int bk_a2_amax(const float *x, long n, float *out);
int bk_a2_quant256(const float *x, long n, float id,
                   signed char *q8, int *s16);
int bk_a2_dn_recur(float *S, const float *k, const float *q, const float *v,
                   float *kvd, float *o, int hd,
                   float decay, float beta, float scale);

#ifdef INFER_HAVE_VIS
/* VIS 1 kernels for UltraSPARC. See backend_vis.c. */
void bk_qmv_vis(int type, const unsigned char *w, const float *x, float *y,
                long ncols, long nrows);
int  bk_vis_available(void);
void bk_vis_free_scratch(void);
#endif

/* Shared by the i8 and mmx backends: quantise the activation vector
 * into 32-element blocks of int8 plus a per-block scale and sum. */
/* Shared by the i8 and mmx backends: the activation vector quantised
 * into 32-element blocks. Values are int8 in range but stored widened
 * to short, so the MMX kernel can feed PMADDWD directly and the scalar
 * kernel needs no unpacking. */
typedef struct {
    short *q;     /* n values, each in [-127,127], padded to a multiple of 32 */
    signed char *q8; /* the same values as int8, for VPMADDUBSW (AVX2)        */
    /* Q8_K-style plane: ONE scale per 256 activations instead of one
     * per 32. The k-quant AVX2 kernels fold the weight scale into the
     * integer accumulator and convert to float once per 256-weight
     * superblock -- which is only possible if the activation scale is
     * constant across that superblock. Built alongside the 32-block
     * plane; the two are independent quantisations of the same x. */
    signed char *k8;   /* n int8 values, scale constant per 256          */
    float       *kd;   /* per-256 scale                                  */
    int         *ks16; /* per-16 sums of k8 (k-quant min / bias terms)   */
    float *d;     /* per-block scale                                          */
    int   *s;     /* per-block sum of q (for the k-quant minimum term)        */
    int   *s16;   /* per-16-elements sum of q (Q6_K scale granularity)        */
    long   n;
} bk_qx;

const bk_qx *bk_quantize_x(const float *x, long n);
/* Latch the quantised activation so re-entrant calls from thread-pool
 * workers return the existing buffer instead of racing to rewrite it.
 * Strictly paired around a parallel region. */
/* Build (once) and return the Q8_K-style plane described above. */
const bk_qx *bk_quantize_k(const float *x, long n);
void         bk_quantize_hold(void);
void         bk_quantize_release(void);
void         bk_free_scratch(void);

/* ------------------------------------------------------------------ */
/* Per-format kernel selection                                         */
/*                                                                     */
/* `--backend` chooses one kernel for every weight format. That is the */
/* right default, but it is not always the fastest arrangement: the    */
/* formats have different shapes and a kernel that wins on one can     */
/* lose on another. Q6_K on UltraSPARC is the worked example -- the    */
/* VIS kernel cuts instructions 3.1x and measured 0.8% (finding 30).   */
/*                                                                     */
/* All kernels are bit-identical to each other on every format, so     */
/* mixing them is a pure performance choice with no numerical effect.  */
/* Asserted by tests/t_ident.c.                                        */
/*                                                                     */
/*   --kernel q4k=mmx        pin one format                            */
/*   --kernel bench          measure on THIS machine and use the       */
/*                           winner for each format                    */
/*   --kernel list           show the current assignment               */
/* ------------------------------------------------------------------ */

/* Formats worth distinguishing. Anything else falls back to whatever
 * --backend selected. */
enum {
    BK_F_Q4_K = 0,
    BK_F_Q5_K,
    BK_F_Q6_K,
    BK_F_Q8_0,
    BK_F_NFORMATS
};

/* Pin one format to one backend. Returns 0, -1 on an unknown format or
 * backend name, -2 if the backend exists but this CPU cannot run it. */
int  bk_kernel_set(const char *format, const char *backend);

/* Parse a --kernel argument: "q4k=mmx", "bench", "list", or "auto". */
int  bk_kernel_arg(const char *arg);

/* Time every available kernel on every format and pin the winners.
 * Uses synthetic blocks, so it needs no model and touches no weights.
 * Prints a table when verbose. */
void bk_kernel_bench(int verbose);

/* Print the current per-format assignment. */
void bk_kernel_list(FILE *f);

#endif /* INFER_BACKEND_H */
