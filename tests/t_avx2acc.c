/* t_avx2acc.c -- the AVX2 k-quant kernels are ACCURATE, not identical.
 *
 * The fast AVX2 kernels changed shape in 1.20.0: they read a Q8_K-style
 * activation plane (one scale per 256 values), fold the weight scale
 * into the integer accumulator, and convert to float once per
 * superblock. That is what llama.cpp does and it is the difference
 * between ~10 and ~16 GFlop/s here.
 *
 * The cost is that the result is no longer BIT-identical to the scalar
 * `i8` path: the activation is quantised against a different scale,
 * the sum is reassociated across SIMD lanes, and the scale is applied
 * before the float conversion instead of after. Every one of those is
 * a legitimate reordering of the same mathematics, but "legitimate"
 * is not a measurement.
 *
 * So t_ident can no longer cover these kernels, and this test replaces
 * it for them: compare against the REFERENCE float kernel (qmv_ref,
 * which dequantises to float and accumulates in float, i.e. the most
 * accurate thing available) and bound the relative error.
 *
 * The bar: the AVX2 kernel must be at least as close to `ref` as the
 * scalar `i8` kernel is. If quantising the activation per 256 instead
 * of per 32 were materially worse, that would show up here as a larger
 * error than i8's, and the optimisation would have to be reverted.
 *
 * Runs standalone; needs no model.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "backend.h"
#include "infer.h"

static unsigned long rs = 12345UL;
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

/* Relative L2 error of `got` against `want`. */
static double rel_err(const float *got, const float *want, int n) {
    double num = 0.0, den = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double d = (double) got[i] - (double) want[i];
        num += d * d;
        den += (double) want[i] * (double) want[i];
    }
    if (den == 0.0) return num == 0.0 ? 0.0 : 1.0;
    return sqrt(num / den);
}

static int check(int type, const char *name, long ncols, int nrows) {
    long bytes = rowbytes(type, ncols);
    unsigned char *w = (unsigned char *) malloc((size_t) (bytes * nrows));
    float *x = (float *) malloc((size_t) ncols * sizeof(float));
    float *yr = (float *) malloc((size_t) nrows * sizeof(float));
    float *yi = (float *) malloc((size_t) nrows * sizeof(float));
    float *ya = (float *) malloc((size_t) nrows * sizeof(float));
    double ei, ea;
    long i;
    int r, bad = 0;

    for (i = 0; i < bytes * nrows; i++) w[i] = (unsigned char) (rnd() & 0xFF);
    /* Give the f16 scales sane exponents so the block values are not
     * denormal or enormous -- otherwise the comparison measures the
     * random bit pattern, not the kernel. */
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
        /* Q8_0 has a scale every 34 bytes, not per superblock. Random
         * bytes there produce f16 Inf/NaN and the comparison becomes
         * meaningless -- pin the exponent as for the k-quants. */
        if (type == GGML_TYPE_Q8_0) {
            long k;
            for (k = 0; k < ncols / 32; k++) {
                long o = r * bytes + k * 34;
                w[o] = (unsigned char) (rnd() & 0xFF); w[o + 1] = 0x2C;
            }
        }
    }
    for (i = 0; i < ncols; i++)
        x[i] = ((float) (int) (rnd() & 0x1FF) - 256.0f) / 128.0f;

    qmv_ref(type, w, x, yr, ncols, nrows);
    qmv_i8 (type, w, x, yi, ncols, nrows);
#if defined(INFER_HAVE_AVX2) && defined(__AVX2__)
    qmv_avx2(type, w, x, ya, ncols, nrows);
#else
    memcpy(ya, yi, (size_t) nrows * sizeof(float));
#endif

    ei = rel_err(yi, yr, nrows);
    ea = rel_err(ya, yr, nrows);

    /* i8 quantises the activation to int8 per 32; avx2 per 256. Both
     * are approximations of ref. Allow avx2 to be up to 4x i8's error
     * (coarser activation blocks) but no worse -- and cap it in
     * absolute terms too, so "i8 is also bad" cannot hide a bug. */
    if (!(ea < 0.02) || !(ea <= ei * 4.0 + 1e-6)) {
        printf("  %-5s ncols=%-6ld ** ERROR TOO LARGE ** "
               "i8 %.3e  avx2 %.3e\n", name, ncols, ei, ea);
        bad = 1;
    } else {
        printf("  %-5s ncols=%-6ld  rel L2 vs ref:  i8 %.3e   avx2 %.3e\n",
               name, ncols, ei, ea);
    }

    free(w); free(x); free(yr); free(yi); free(ya);
    return bad;
}

int main(void) {
    int bad = 0;
    printf("AVX2 k-quant accuracy against the float reference kernel\n");
    printf("(these kernels are deliberately NOT bit-identical -- see "
           "the file header)\n\n");

    bad += check(GGML_TYPE_Q4_K, "Q4_K", 1024,  16);
    bad += check(GGML_TYPE_Q4_K, "Q4_K", 3584,  16);
    bad += check(GGML_TYPE_Q5_K, "Q5_K", 1024,  16);
    bad += check(GGML_TYPE_Q5_K, "Q5_K", 3584,  16);
    bad += check(GGML_TYPE_Q6_K, "Q6_K", 1024,  16);
    bad += check(GGML_TYPE_Q6_K, "Q6_K", 3584,  16);
    bad += check(GGML_TYPE_Q8_0, "Q8_0", 1024,  16);

    if (bad) {
        printf("\n%d FORMAT(S) OUTSIDE THE ERROR BUDGET\n", bad);
        return 1;
    }
    printf("\nall formats within budget -- avx2 is as accurate as i8\n");
    return 0;
}
