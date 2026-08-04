/* Compare the VIS kernels against the scalar reference on synthetic
 * blocks. No model needed. Built once per compiler path. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "backend.h"
#include "infer.h"

static unsigned long rs = 12345UL;
static unsigned int rnd(void) {
    rs = rs * 1103515245UL + 12345UL;
    return (unsigned int) ((rs >> 16) & 0xFFFFU);
}

static int check(int type, const char *name, long ncols, int nrows) {
    long rowbytes = (type == GGML_TYPE_Q4_K) ? (ncols / 256) * 144
                                             : (ncols / 256) * 176;
    unsigned char *w = (unsigned char *) malloc((size_t)(rowbytes * nrows));
    float *x  = (float *) malloc((size_t) ncols * sizeof(float));
    float *y1 = (float *) malloc((size_t) nrows * sizeof(float));
    float *y2 = (float *) malloc((size_t) nrows * sizeof(float));
    long i; int r; double worst = 0.0; int bad = 0;

    for (i = 0; i < rowbytes * nrows; i++) w[i] = (unsigned char) (rnd() & 0xFF);
    /* keep the fp16 scales sane: small positive exponents */
    for (r = 0; r < nrows; r++) {
        long base = r * rowbytes; long blk;
        for (blk = 0; blk < ncols / 256; blk++) {
            long o = base + blk * ((type == GGML_TYPE_Q4_K) ? 144 : 176);
            w[o]   = (unsigned char)(rnd() & 0xFF); w[o+1] = 0x2C; /* ~0.06 */
            w[o+2] = (unsigned char)(rnd() & 0xFF); w[o+3] = 0x2B;
        }
    }
    for (i = 0; i < ncols; i++)
        x[i] = ((float) (int) (rnd() & 0x1FF) - 256.0f) / 128.0f;

    qmv_i8(type, w, x, y1, ncols, nrows);
    bk_qmv_vis(type, w, x, y2, ncols, nrows);

    for (r = 0; r < nrows; r++) {
        double d = fabs((double) y1[r] - (double) y2[r]);
        double m = fabs((double) y1[r]); double rel = m > 1e-6 ? d / m : d;
        if (rel > worst) worst = rel;
        if (rel > 1e-4) bad++;
    }
    printf("%-8s ncols=%-5ld rows=%-3d  worst rel err %.3e  %s\n",
           name, ncols, nrows, worst, bad ? "** FAIL **" : "ok");
    free(w); free(x); free(y1); free(y2);
    return bad;
}

int main(void) {
    int bad = 0;
    printf("VIS kernels vs i8 scalar kernel (same quantisation)\n\n");
    if (!bk_vis_available()) { printf("VIS not available\n"); return 1; }
    bad += check(GGML_TYPE_Q4_K, "Q4_K", 256, 4);
    bad += check(GGML_TYPE_Q4_K, "Q4_K", 1024, 8);
    bad += check(GGML_TYPE_Q4_K, "Q4_K", 3584, 4);
    bad += check(GGML_TYPE_Q5_K, "Q5_K", 256, 4);
    bad += check(GGML_TYPE_Q5_K, "Q5_K", 1024, 8);
    bad += check(GGML_TYPE_Q5_K, "Q5_K", 3584, 4);
    printf(bad ? "\n%d FAILURE(S)\n" : "\nkernels agree with the reference\n", bad);
    return bad ? 1 : 0;
}
