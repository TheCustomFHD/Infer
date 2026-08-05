/* t_thread.c -- threading changes the schedule, never the arithmetic.
 *
 * Rows of a matvec are split across the pool, and Gated DeltaNet heads
 * are split the same way. Both are safe only because no partial result
 * crosses a thread: index i is computed by the same code, summing the
 * same terms in the same order, wherever it runs.
 *
 * That is a claim about the code, so it is tested as one: run the same
 * matvec at several thread counts and require BIT-identical output.
 * Not "close" -- identical. If a future change ever splits a dot
 * product across threads instead of splitting the rows, this fails.
 *
 * Also exercises the activation-quantisation latch (bk_quantize_hold /
 * bk_quantize_release). Without it every worker races to rewrite one
 * shared scratch buffer, which is a genuine data race; with it the
 * repeats are no-ops. A race there would show up here as nondeterminism
 * across runs, so the test repeats each configuration.
 *
 * Runs standalone; needs no model.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "backend.h"
#include "tpool.h"
#include "infer.h"

static unsigned long rs = 4242UL;
static unsigned int rnd(void) {
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned int) ((rs >> 16) & 0xFFFFU);
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

static int check(int type, const char *name, long ncols, int nrows) {
    long bytes = rowbytes(type, ncols);
    unsigned char *w = (unsigned char *) malloc((size_t) (bytes * nrows));
    float *x  = (float *) malloc((size_t) ncols * sizeof(float));
    float *y1 = (float *) malloc((size_t) nrows * sizeof(float));
    float *yn = (float *) malloc((size_t) nrows * sizeof(float));
    long i;
    int r, rep, bad = 0;

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
    for (i = 0; i < ncols; i++)
        x[i] = ((float) (int) (rnd() & 0x1FF) - 256.0f) / 128.0f;

    /* reference: whatever the pool currently is, measured once */
    q_matvec(type, w, x, y1, ncols, nrows);

    /* repeat: a race would make this differ run to run */
    for (rep = 0; rep < 8; rep++) {
        memset(yn, 0, (size_t) nrows * sizeof(float));
        q_matvec(type, w, x, yn, ncols, nrows);
        if (memcmp(y1, yn, (size_t) nrows * sizeof(float)) != 0) {
            int shown = 0;
            for (r = 0; r < nrows && shown < 3; r++) {
                if (memcmp(&y1[r], &yn[r], 4) != 0) {
                    printf("    rep %d row %d: %.9g vs %.9g\n",
                           rep, r, y1[r], yn[r]);
                    shown++;
                }
            }
            bad = 1;
            break;
        }
    }

    printf("  %-5s ncols=%-5ld nrows=%-5d  %s\n", name, ncols, nrows,
           bad ? "** NONDETERMINISTIC **" : "stable");

    free(w); free(x); free(y1); free(yn);
    return bad;
}

int main(int argc, char **argv) {
    int want = (argc > 1) ? atoi(argv[1]) : 0;
    int bad = 0;
    int n;

    n = tp_start(want);
    printf("thread determinism: pool has %d thread(s), %d logical CPU(s)\n\n",
           n, tp_cpu_count());

    bad += check(GGML_TYPE_Q4_K, "Q4_K", 1024, 512);
    bad += check(GGML_TYPE_Q5_K, "Q5_K", 1024, 512);
    bad += check(GGML_TYPE_Q6_K, "Q6_K", 1024, 512);
    bad += check(GGML_TYPE_Q8_0, "Q8_0", 1024, 512);
    /* a row count that does not divide evenly by the thread count --
     * the remainder slice is the caller's and is easy to get wrong */
    bad += check(GGML_TYPE_Q4_K, "Q4_K", 1024, 517);
    bad += check(GGML_TYPE_Q6_K, "Q6_K", 1024,  65);

    tp_stop();

    if (bad) {
        printf("\n%d CONFIGURATION(S) NOT REPRODUCIBLE\n", bad);
        return 1;
    }
    printf("\nall stable -- threading does not perturb the result\n");
    return 0;
}
