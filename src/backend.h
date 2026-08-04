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
    float *d;     /* per-block scale                                          */
    int   *s;     /* per-block sum of q (for the k-quant minimum term)        */
    int   *s16;   /* per-16-elements sum of q (Q6_K scale granularity)        */
    long   n;
} bk_qx;

const bk_qx *bk_quantize_x(const float *x, long n);
void         bk_free_scratch(void);

#endif /* INFER_BACKEND_H */
