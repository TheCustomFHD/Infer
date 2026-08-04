/* Compare the VIS kernels against the scalar reference on synthetic
 * blocks. No model needed. Built once per compiler path.
 *
 * The 256-column case deliberately misaligns Q6_K and Q8_0 rows (their
 * row strides are 210*nb and 34*nb bytes, neither a multiple of four
 * for odd nb), forcing the alignment-safe load paths.
 */
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

static long type_rowbytes(int type, long ncols) {
    long nb = ncols / 256;
    switch (type) {
        case GGML_TYPE_Q4_K: return nb * 144;
        case GGML_TYPE_Q5_K: return nb * 176;
        case GGML_TYPE_Q6_K: return nb * 210;
        case GGML_TYPE_Q8_0: return (ncols / 32) * 34;
        case GGML_TYPE_Q4_0: return (ncols / 32) * 18;
        default:             return 0;
    }
}

static int check(int type, const char *name, long ncols, int nrows) {
    long rowbytes = type_rowbytes(type, ncols);
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
            long o;
            switch (type) {
                case GGML_TYPE_Q4_K: o = base + blk * 144; break;
                case GGML_TYPE_Q5_K: o = base + blk * 176; break;
                case GGML_TYPE_Q6_K: o = base + blk * 210; break;
                default:             o = base; break;
            }
            w[o]   = (unsigned char)(rnd() & 0xFF); w[o+1] = 0x2C; /* ~0.06 */
            if (type != GGML_TYPE_Q8_0 && type != GGML_TYPE_Q4_0) {
                w[o+2] = (unsigned char)(rnd() & 0xFF); w[o+3] = 0x2B;
            }
        }
        /* Q8_0 and Q4_0 have 32-element blocks; give every block a
         * sane fp16 scale so no comparison is skipped by a NaN. */
        if (type == GGML_TYPE_Q8_0 || type == GGML_TYPE_Q4_0) {
            long step = (type == GGML_TYPE_Q8_0) ? 34 : 18;
            for (blk = 0; blk < ncols / 32; blk++) {
                long o = base + blk * step;
                w[o]   = (unsigned char)(rnd() & 0xFF); w[o+1] = 0x2C;
            }
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
    bad += check(GGML_TYPE_Q6_K, "Q6_K", 256, 4);
    bad += check(GGML_TYPE_Q6_K, "Q6_K", 1024, 8);
    bad += check(GGML_TYPE_Q6_K, "Q6_K", 3584, 4);
    bad += check(GGML_TYPE_Q8_0, "Q8_0", 256, 4);
    bad += check(GGML_TYPE_Q8_0, "Q8_0", 1024, 8);
    bad += check(GGML_TYPE_Q8_0, "Q8_0", 3584, 4);
    bad += check(GGML_TYPE_Q4_0, "Q4_0", 256, 4);
    bad += check(GGML_TYPE_Q4_0, "Q4_0", 1024, 8);
    bad += check(GGML_TYPE_Q4_0, "Q4_0", 3584, 4);
    printf(bad ? "\n%d FAILURE(S)\n" : "\nkernels agree with the reference\n", bad);
    return bad ? 1 : 0;
}
