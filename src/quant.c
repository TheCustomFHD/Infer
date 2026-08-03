/* quant.c -- GGML block-quantisation decoders and matrix-vector kernels.
 *
 * Implements the k-quant formats actually present in a Q4_K_M build of
 * Qwen3.5 (Q4_K, Q5_K, Q6_K, Q8_0) plus F32/F16 and the legacy Q4_0/Q8_0
 * blocks, so other quantisations of the same model still load.
 *
 * Everything is scalar C89 -- no SSE, no MMX, no x87 intrinsics -- so the
 * object code runs on a plain i486. The kernels are written to keep the
 * inner loops branch-free and sequential, which is what matters most on
 * an in-order 486 pipeline with a small cache.
 *
 * Block layouts follow ggml-common.h exactly:
 *
 *   Q4_0  (32 elems, 18 B):  fp16 d | 16 B nibbles          q = d*(n-8)
 *   Q8_0  (32 elems, 34 B):  fp16 d | 32 B int8             q = d*n
 *   Q4_K  (256 elems,144 B): fp16 d | fp16 dmin | 12 B scales | 128 B nibbles
 *   Q5_K  (256 elems,176 B): fp16 d | fp16 dmin | 12 B scales | 32 B hi bits
 *                            | 128 B nibbles
 *   Q6_K  (256 elems,210 B): 128 B low nibbles | 64 B high bits
 *                            | 16 B int8 scales | fp16 d
 */

#include "infer.h"
#include "backend.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* fp16 -> fp32                                                        */
/* ------------------------------------------------------------------ */

float q_fp16_to_fp32(unsigned short h) {
    unsigned long sign = (unsigned long) (h >> 15) & 1UL;
    unsigned long expo = (unsigned long) (h >> 10) & 0x1FUL;
    unsigned long mant = (unsigned long) h & 0x3FFUL;
    unsigned long bits;
    float out;

    if (expo == 0) {
        if (mant == 0) {
            bits = sign << 31;                       /* +/- zero */
        } else {
            /* subnormal: normalise */
            expo = 127 - 15 + 1;
            while ((mant & 0x400UL) == 0) {
                mant <<= 1;
                expo--;
            }
            mant &= 0x3FFUL;
            bits = (sign << 31) | (expo << 23) | (mant << 13);
        }
    } else if (expo == 31) {
        bits = (sign << 31) | (255UL << 23) | (mant << 13);  /* inf / nan */
    } else {
        bits = (sign << 31) | ((expo - 15 + 127) << 23) | (mant << 13);
    }

    /* Narrow to exactly 32 bits before the copy.
     *
     * `bits` is an unsigned long -- 8 bytes on LP64. A plain
     * memcpy(&out, &bits, 4) therefore copied the *high* half on a
     * big-endian 64-bit host (s390x, ppc64), which silently zeroed
     * every quantisation scale in the model: the file parsed, the
     * tensors loaded, and generation produced pure garbage.
     *
     * Going through a fixed 4-byte object fixes it on any host and any
     * long width, and costs nothing: on both byte orders this still
     * compiles to a single register-to-register move. Extracting the
     * four bytes by hand instead would be equally correct but stops
     * the compiler folding it, which measured ~4% end-to-end. */
    {
        unsigned int b32 = (unsigned int) (bits & 0xFFFFFFFFUL);
        memcpy(&out, &b32, 4);
    }
    return out;
}

static float rd_f16p(const unsigned char *p) {
    return q_fp16_to_fp32((unsigned short) (p[0] | (p[1] << 8)));
}

/* ------------------------------------------------------------------ */
/* k-quant 6-bit scale/min unpacking                                   */
/* ------------------------------------------------------------------ */

/* Q4_K and Q5_K pack 8 scales and 8 mins as 6-bit values into 12 bytes.
 * This mirrors get_scale_min_k4() from ggml-quants.c. */
static void get_scale_min_k4(int j, const unsigned char *q,
                             unsigned char *d, unsigned char *m) {
    if (j < 4) {
        *d = (unsigned char) (q[j] & 63);
        *m = (unsigned char) (q[j + 4] & 63);
    } else {
        *d = (unsigned char) ((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        *m = (unsigned char) ((q[j + 4] >> 4)  | ((q[j] >> 6) << 4));
    }
}

/* ------------------------------------------------------------------ */
/* Per-block dequantisation                                            */
/* ------------------------------------------------------------------ */

static void deq_q4_K(const unsigned char *b, float *y) {
    const float d    = rd_f16p(b);
    const float dmin = rd_f16p(b + 2);
    const unsigned char *sc = b + 4;
    const unsigned char *qs = b + 16;
    int j, l;

    for (j = 0; j < QK_K / 64; j++) {
        unsigned char sc1, m1, sc2, m2;
        float d1, min1, d2, min2;

        get_scale_min_k4(j * 2,     sc, &sc1, &m1);
        get_scale_min_k4(j * 2 + 1, sc, &sc2, &m2);
        d1   = d * (float) sc1;
        min1 = dmin * (float) m1;
        d2   = d * (float) sc2;
        min2 = dmin * (float) m2;

        for (l = 0; l < 32; l++) {
            y[l]      = d1 * (float) (qs[l] & 0xF) - min1;
            y[l + 32] = d2 * (float) (qs[l] >> 4)  - min2;
        }
        y  += 64;
        qs += 32;
    }
}

static void deq_q5_K(const unsigned char *b, float *y) {
    const float d    = rd_f16p(b);
    const float dmin = rd_f16p(b + 2);
    const unsigned char *sc = b + 4;
    const unsigned char *qh = b + 16;
    const unsigned char *qs = b + 48;
    int j, l;
    unsigned char u1 = 1, u2 = 2;

    for (j = 0; j < QK_K / 64; j++) {
        unsigned char sc1, m1, sc2, m2;
        float d1, min1, d2, min2;

        get_scale_min_k4(j * 2,     sc, &sc1, &m1);
        get_scale_min_k4(j * 2 + 1, sc, &sc2, &m2);
        d1   = d * (float) sc1;
        min1 = dmin * (float) m1;
        d2   = d * (float) sc2;
        min2 = dmin * (float) m2;

        for (l = 0; l < 32; l++) {
            int lo = qs[l] & 0xF;
            int hi = (qh[l] & u1) ? 16 : 0;
            y[l] = d1 * (float) (lo + hi) - min1;
        }
        for (l = 0; l < 32; l++) {
            int lo = qs[l] >> 4;
            int hi = (qh[l] & u2) ? 16 : 0;
            y[l + 32] = d2 * (float) (lo + hi) - min2;
        }
        y  += 64;
        qs += 32;
        u1 = (unsigned char) (u1 << 2);
        u2 = (unsigned char) (u2 << 2);
    }
}

static void deq_q6_K(const unsigned char *b, float *y) {
    const unsigned char *ql = b;
    const unsigned char *qh = b + 128;
    const signed char   *sc = (const signed char *) (b + 192);
    const float d = rd_f16p(b + 208);
    int n, l;

    for (n = 0; n < QK_K; n += 128) {
        for (l = 0; l < 32; l++) {
            int is = l / 16;
            int q1 = (int) ((ql[l]      & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
            int q2 = (int) ((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
            int q3 = (int) ((ql[l]      >> 4)  | (((qh[l] >> 4) & 3) << 4)) - 32;
            int q4 = (int) ((ql[l + 32] >> 4)  | (((qh[l] >> 6) & 3) << 4)) - 32;

            y[l]      = d * (float) sc[is]     * (float) q1;
            y[l + 32] = d * (float) sc[is + 2] * (float) q2;
            y[l + 64] = d * (float) sc[is + 4] * (float) q3;
            y[l + 96] = d * (float) sc[is + 6] * (float) q4;
        }
        y  += 128;
        ql += 64;
        qh += 32;
        sc += 8;
    }
}

static void deq_q8_0(const unsigned char *b, float *y) {
    const float d = rd_f16p(b);
    const signed char *q = (const signed char *) (b + 2);
    int i;
    for (i = 0; i < 32; i++) y[i] = d * (float) q[i];
}

static void deq_q4_0(const unsigned char *b, float *y) {
    const float d = rd_f16p(b);
    const unsigned char *q = b + 2;
    int i;
    for (i = 0; i < 16; i++) {
        y[i]      = d * (float) ((int) (q[i] & 0xF) - 8);
        y[i + 16] = d * (float) ((int) (q[i] >> 4)  - 8);
    }
}

/* ------------------------------------------------------------------ */
/* Row dequantisation                                                  */
/* ------------------------------------------------------------------ */

void q_dequant_row(int type, const unsigned char *src, float *dst, long n) {
    long i;

    switch (type) {
        case GGML_TYPE_F32:
            inf_rd_f32v(src, dst, n);
            return;

        case GGML_TYPE_F16:
            for (i = 0; i < n; i++) dst[i] = rd_f16p(src + i * 2);
            return;

        case GGML_TYPE_Q4_K:
            for (i = 0; i < n; i += QK_K) {
                deq_q4_K(src, dst + i);
                src += 144;
            }
            return;

        case GGML_TYPE_Q5_K:
            for (i = 0; i < n; i += QK_K) {
                deq_q5_K(src, dst + i);
                src += 176;
            }
            return;

        case GGML_TYPE_Q6_K:
            for (i = 0; i < n; i += QK_K) {
                deq_q6_K(src, dst + i);
                src += 210;
            }
            return;

        case GGML_TYPE_Q8_0:
            for (i = 0; i < n; i += 32) {
                deq_q8_0(src, dst + i);
                src += 34;
            }
            return;

        case GGML_TYPE_Q4_0:
            for (i = 0; i < n; i += 32) {
                deq_q4_0(src, dst + i);
                src += 18;
            }
            return;

        default:
            for (i = 0; i < n; i++) dst[i] = 0.0f;
            return;
    }
}

/* ------------------------------------------------------------------ */
/* Fused dot products: dequantise one block, consume it immediately     */
/*                                                                      */
/* Keeping the dequantised values in registers / L1 rather than writing  */
/* a whole row to memory first is a large win on a 486, where memory     */
/* bandwidth is the bottleneck and the L1 cache is 8 KB.                 */
/* ------------------------------------------------------------------ */

static float dot_q4_K(const unsigned char *b, const float *x, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = rd_f16p(b);
        const float dmin = rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qs = b + 16;
        int j, l;

        for (j = 0; j < QK_K / 64; j++) {
            unsigned char sc1, m1, sc2, m2;
            float d1, min1, d2, min2;
            float a1 = 0.0f, a2 = 0.0f;   /* sum q_i * x_i           */
            float s1 = 0.0f, s2 = 0.0f;   /* sum x_i (for the min)   */

            get_scale_min_k4(j * 2,     sc, &sc1, &m1);
            get_scale_min_k4(j * 2 + 1, sc, &sc2, &m2);

            for (l = 0; l < 32; l++) {
                float xl = x[l];
                float xh = x[l + 32];
                a1 += (float) (qs[l] & 0xF) * xl;
                s1 += xl;
                a2 += (float) (qs[l] >> 4) * xh;
                s2 += xh;
            }

            d1   = d * (float) sc1;
            min1 = dmin * (float) m1;
            d2   = d * (float) sc2;
            min2 = dmin * (float) m2;

            sum += d1 * a1 - min1 * s1;
            sum += d2 * a2 - min2 * s2;

            x  += 64;
            qs += 32;
        }
        b += 144;
    }
    return sum;
}

static float dot_q5_K(const unsigned char *b, const float *x, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = rd_f16p(b);
        const float dmin = rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qh = b + 16;
        const unsigned char *qs = b + 48;
        unsigned char u1 = 1, u2 = 2;
        int j, l;

        for (j = 0; j < QK_K / 64; j++) {
            unsigned char sc1, m1, sc2, m2;
            float d1, min1, d2, min2;
            float a1 = 0.0f, a2 = 0.0f;
            float s1 = 0.0f, s2 = 0.0f;

            get_scale_min_k4(j * 2,     sc, &sc1, &m1);
            get_scale_min_k4(j * 2 + 1, sc, &sc2, &m2);

            for (l = 0; l < 32; l++) {
                float xl = x[l];
                int   q  = (int) (qs[l] & 0xF) + ((qh[l] & u1) ? 16 : 0);
                a1 += (float) q * xl;
                s1 += xl;
            }
            for (l = 0; l < 32; l++) {
                float xh = x[l + 32];
                int   q  = (int) (qs[l] >> 4) + ((qh[l] & u2) ? 16 : 0);
                a2 += (float) q * xh;
                s2 += xh;
            }

            d1   = d * (float) sc1;
            min1 = dmin * (float) m1;
            d2   = d * (float) sc2;
            min2 = dmin * (float) m2;

            sum += d1 * a1 - min1 * s1;
            sum += d2 * a2 - min2 * s2;

            x  += 64;
            qs += 32;
            u1 = (unsigned char) (u1 << 2);
            u2 = (unsigned char) (u2 << 2);
        }
        b += 176;
    }
    return sum;
}

static float dot_q6_K(const unsigned char *b, const float *x, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const unsigned char *ql = b;
        const unsigned char *qh = b + 128;
        const signed char   *sc = (const signed char *) (b + 192);
        const float d = rd_f16p(b + 208);
        int n, l;
        float acc = 0.0f;

        for (n = 0; n < 2; n++) {
            /* 8 scale groups of 16 values each per 128-element half */
            float g[8];
            int k;
            for (k = 0; k < 8; k++) g[k] = 0.0f;

            for (l = 0; l < 32; l++) {
                int is = l / 16;
                int q1 = (int) ((ql[l]      & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = (int) ((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = (int) ((ql[l]      >> 4)  | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = (int) ((ql[l + 32] >> 4)  | (((qh[l] >> 6) & 3) << 4)) - 32;

                g[is]     += (float) q1 * x[l];
                g[is + 2] += (float) q2 * x[l + 32];
                g[is + 4] += (float) q3 * x[l + 64];
                g[is + 6] += (float) q4 * x[l + 96];
            }
            for (k = 0; k < 8; k++) acc += (float) sc[k] * g[k];

            x  += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
        sum += d * acc;
        b   += 210;
    }
    return sum;
}

static float dot_q8_0(const unsigned char *b, const float *x, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += 32) {
        const float d = rd_f16p(b);
        const signed char *q = (const signed char *) (b + 2);
        float acc = 0.0f;
        int i;
        for (i = 0; i < 32; i++) acc += (float) q[i] * x[i];
        sum += d * acc;
        x += 32;
        b += 34;
    }
    return sum;
}

static float dot_q4_0(const unsigned char *b, const float *x, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += 32) {
        const float d = rd_f16p(b);
        const unsigned char *q = b + 2;
        float acc = 0.0f;
        int i;
        for (i = 0; i < 16; i++) {
            acc += (float) ((int) (q[i] & 0xF) - 8) * x[i];
            acc += (float) ((int) (q[i] >> 4)  - 8) * x[i + 16];
        }
        sum += d * acc;
        x += 32;
        b += 18;
    }
    return sum;
}

static float dot_f32(const unsigned char *b, const float *x, long ncols) {
    float sum = 0.0f;
    long i;
#if INFER_BIG_ENDIAN
    for (i = 0; i < ncols; i++) sum += inf_rd_f32p(b + i * 4) * x[i];
#else
    const float *w = (const float *) b;
    for (i = 0; i < ncols; i++) sum += w[i] * x[i];
#endif
    return sum;
}

static float dot_f16(const unsigned char *b, const float *x, long ncols) {
    float sum = 0.0f;
    long i;
    for (i = 0; i < ncols; i++) sum += rd_f16p(b + i * 2) * x[i];
    return sum;
}

/* ------------------------------------------------------------------ */
/* Matrix-vector product                                               */
/* ------------------------------------------------------------------ */

/* The portable scalar reference kernel. Registered as the "ref" backend
 * in backend.c, which owns the public q_matvec() dispatcher. */
void qmv_ref(int type, const unsigned char *w, const float *x,
             float *y, long ncols, long nrows) {
    long rowbytes = gg_type_size(type, ncols);
    long r;

    switch (type) {
        case GGML_TYPE_Q4_K:
            for (r = 0; r < nrows; r++) y[r] = dot_q4_K(w + r * rowbytes, x, ncols);
            return;
        case GGML_TYPE_Q5_K:
            for (r = 0; r < nrows; r++) y[r] = dot_q5_K(w + r * rowbytes, x, ncols);
            return;
        case GGML_TYPE_Q6_K:
            for (r = 0; r < nrows; r++) y[r] = dot_q6_K(w + r * rowbytes, x, ncols);
            return;
        case GGML_TYPE_Q8_0:
            for (r = 0; r < nrows; r++) y[r] = dot_q8_0(w + r * rowbytes, x, ncols);
            return;
        case GGML_TYPE_Q4_0:
            for (r = 0; r < nrows; r++) y[r] = dot_q4_0(w + r * rowbytes, x, ncols);
            return;
        case GGML_TYPE_F32:
            for (r = 0; r < nrows; r++) y[r] = dot_f32(w + r * rowbytes, x, ncols);
            return;
        case GGML_TYPE_F16:
            for (r = 0; r < nrows; r++) y[r] = dot_f16(w + r * rowbytes, x, ncols);
            return;
        default:
            for (r = 0; r < nrows; r++) y[r] = 0.0f;
            return;
    }
}
