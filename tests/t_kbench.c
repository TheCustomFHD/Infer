/* t_kbench.c -- isolated per-format kernel throughput.
 *
 * The whole-model profile mixes matvec with quantisation, threading and
 * memory traffic, so a kernel change of 20% can vanish into the noise
 * or -- worse -- look like a regression caused by something else.
 * This times ONE kernel on ONE format with everything else held still,
 * which is what a kernel edit actually needs to be judged against.
 *
 * Reports ns per weight, which is the only unit comparable across
 * formats: a Q6_K call carries 6x the weights of a Q4_K call, so
 * us/call says nothing (finding 30).
 *
 * Runs standalone; needs no model.
 *
 *   ./t_kbench            all formats, all available backends
 *   ./t_kbench 4096 64    ncols nrows
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "backend.h"
#include "infer.h"

static unsigned long rs = 777UL;
static unsigned int rnd(void) {
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned int) ((rs >> 16) & 0xFFFFU);
}

static double now(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + 1e-9 * (double) t.tv_nsec;
#else
    return (double) clock() / (double) CLOCKS_PER_SEC;
#endif
}

static long rowbytes(int t, long n) {
    long nb = n / 256;
    switch (t) {
        case GGML_TYPE_Q4_K: return nb * 144;
        case GGML_TYPE_Q5_K: return nb * 176;
        case GGML_TYPE_Q6_K: return nb * 210;
        default:             return (n / 32) * 34;
    }
}

static void fill(unsigned char *w, long bytes, int type, long ncols,
                 int nrows) {
    long i;
    int r;
    for (i = 0; i < bytes * nrows; i++) w[i] = (unsigned char) (rnd() & 0xFF);
    for (r = 0; r < nrows; r++) {
        long k;
        for (k = 0; k < ncols / 256; k++) {
            long o = r * bytes + k * (type == GGML_TYPE_Q4_K ? 144 :
                                      (type == GGML_TYPE_Q5_K ? 176 : 210));
            if (type == GGML_TYPE_Q4_K || type == GGML_TYPE_Q5_K) {
                w[o] = (unsigned char) (rnd() & 0xFF); w[o + 1] = 0x2C;
                w[o + 2] = (unsigned char) (rnd() & 0xFF); w[o + 3] = 0x2B;
            } else if (type == GGML_TYPE_Q6_K) {
                w[o + 208] = (unsigned char) (rnd() & 0xFF); w[o + 209] = 0x2C;
            }
        }
        if (type == GGML_TYPE_Q8_0) {
            long k2;
            for (k2 = 0; k2 < ncols / 32; k2++) {
                long o = r * bytes + k2 * 34;
                w[o] = (unsigned char) (rnd() & 0xFF); w[o + 1] = 0x2C;
            }
        }
    }
}

typedef void (*mvfn)(int, const unsigned char *, const float *, float *,
                     long, long);

static void bench_one(const char *bname, mvfn fn, int type, const char *tname,
                      long ncols, int nrows, double *out) {
    long bytes = rowbytes(type, ncols);
    unsigned char *w = (unsigned char *) malloc((size_t) (bytes * nrows));
    float *x = (float *) malloc((size_t) ncols * sizeof(float));
    float *y = (float *) malloc((size_t) nrows * sizeof(float));
    double best = 1e30;
    long i;
    int pass, reps = 1;

    fill(w, bytes, type, ncols, nrows);
    for (i = 0; i < ncols; i++)
        x[i] = ((float) (int) (rnd() & 0x1FF) - 256.0f) / 128.0f;

    /* scale reps so a pass is roughly 30 ms */
    {
        double t0 = now();
        fn(type, w, x, y, ncols, nrows);
        {
            double el = now() - t0;
            if (el > 0.0) reps = (int) (0.03 / el);
            if (reps < 1) reps = 1;
            if (reps > 20000) reps = 20000;
        }
    }

    for (pass = 0; pass < 3; pass++) {
        double t0, el;
        int k;
        t0 = now();
        for (k = 0; k < reps; k++) fn(type, w, x, y, ncols, nrows);
        el = (now() - t0) / (double) reps;
        if (el < best) best = el;
    }

    /* ns per weight -- the only cross-format comparable unit */
    *out = best * 1e9 / ((double) ncols * (double) nrows);
    printf("  %-5s %-5s  %8.3f ns/weight   %8.1f Mflop/s\n",
           tname, bname, *out,
           2.0 * (double) ncols * (double) nrows / best / 1e6);

    free(w); free(x); free(y);
}

int main(int argc, char **argv) {
    long ncols = (argc > 1) ? atol(argv[1]) : 3584;
    int  nrows = (argc > 2) ? atoi(argv[2]) : 64;
    int  types[4];
    const char *names[4];
    int t;

    types[0] = GGML_TYPE_Q4_K; names[0] = "Q4_K";
    types[1] = GGML_TYPE_Q5_K; names[1] = "Q5_K";
    types[2] = GGML_TYPE_Q6_K; names[2] = "Q6_K";
    types[3] = GGML_TYPE_Q8_0; names[3] = "Q8_0";

    printf("kernel bench: ncols=%ld nrows=%d  (lower ns/weight is better)\n\n",
           ncols, nrows);

    for (t = 0; t < 4; t++) {
        double a = 0.0, b = 0.0;
        bench_one("i8", qmv_i8, types[t], names[t], ncols, nrows, &a);
#ifdef INFER_HAVE_MMX
        { double m = 0.0;
          bench_one("mmx", qmv_mmx, types[t], names[t], ncols, nrows, &m); }
#endif
#if defined(INFER_HAVE_AVX2) && defined(__AVX2__)
        bench_one("avx2", qmv_avx2, types[t], names[t], ncols, nrows, &b);
        if (b > 0.0) printf("  %-5s speedup avx2 vs i8: %.2fx\n", names[t], a / b);
#else
        (void) b;
#endif
        printf("\n");
    }
    return 0;
}
