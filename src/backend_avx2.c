/* backend_avx2.c -- AVX2 inner loops for the quantised matrix-vector product.
 *
 * Compiled only when INFER_HAVE_AVX2 is defined AND the compiler is
 * actually targeting AVX2 (__AVX2__). Selected at run time only when
 * CPUID reports AVX2 *and* the OS has enabled ymm state via XGETBV.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Profiling on real hardware (finding 46) put matvec at 87-91% of the
 * forward pass, with an Amdahl ceiling above 7x, while the shipped
 * AVX2 binary reached only 1.37x over the 32-bit MMX baseline. The
 * disassembly said why: GCC autovectorised the portable `i8` loops
 * into 127 ymm references and 18 VPMADDWD, and emitted *zero*
 * VPMADDUBSW. That one instruction is the whole opportunity.
 *
 *   VPMADDWD     16x int16 -> 8x int32   =  16 MACs / instruction
 *   VPMADDUBSW   32x u8xs8 -> 16x int16  =  32 MACs / instruction
 *
 * The weights in every k-quant format are *unsigned* small integers
 * once unpacked -- Q4_K nibbles are 0..15, Q5_K 0..31, Q6_K 0..63 --
 * and the activations are already quantised to int8. That is exactly
 * the (u8, s8) operand pair VPMADDUBSW wants. GCC will not discover
 * this on its own because the portable C widens everything to int
 * before multiplying.
 *
 * SATURATION
 * ----------
 * VPMADDUBSW saturates its int16 result. It multiplies two adjacent
 * (u8, s8) pairs and adds them, so the worst case per output lane is
 * 2 * wmax * 127:
 *
 *   Q4_K  wmax 15   2*15*127 =  3810   int16 max 32767   9 bits spare
 *   Q5_K  wmax 31   2*31*127 =  7874                     6 bits spare
 *   Q6_K  wmax 63   2*63*127 = 16002                     1 bit  spare
 *
 * Q6_K has the least headroom and still cannot saturate. Asserted
 * exhaustively over every (weight, activation) pair by tests/t_avx2sat.c
 * rather than argued -- the VIS work (finding 29) showed lane-budget
 * reasoning is exactly where this kind of kernel goes quietly wrong.
 *
 * BIT-IDENTITY
 * ------------
 * Every accumulator here is a plain int32 sum of the same integer
 * products the scalar kernel forms. Integer addition is associative
 * and none of these sums can overflow, so reassociating them across
 * SIMD lanes is exact, not approximate. The float combination that
 * follows is then performed in the *same order and the same types* as
 * i8_dot_*, so the result is bit-identical rather than merely close.
 * tests/t_ident.c checks this with memcmp, not a tolerance.
 *
 * ANSI C: this file is C89 in structure (declarations first) but uses
 * <immintrin.h>, so like backend_mmx.c (GNU inline asm) and
 * backend_vis.c (vector extensions) it is not strictly conforming. It
 * is optional and excluded from the portable build.
 */

#include "backend.h"
#include "infer.h"

#if defined(INFER_HAVE_AVX2) && defined(__AVX2__)

#include <string.h>
#include <immintrin.h>

#define QK_K 256

/* Prefetch DISTANCE, in superblocks.
 *
 * One block ahead is useless: a 256-weight block takes roughly 26 ns
 * to process and a DRAM miss costs 80-100 ns, so the line arrives long
 * after it was needed. Four blocks ahead covers the latency with a
 * little margin, and the working set stays tiny (4 x ~200 B).
 *
 * Measured on a streaming matvec (the lm head is 209 MB, four times
 * the L3): distance 1 gave 3.77 GB/s, distance 4 gave the numbers in
 * finding 49. Beyond ~6 the lines start evicting each other and it
 * turns back down. */
#define A2_PFDIST 32


/* Prefetch a whole block: 144-210 bytes = 3-4 cache lines. */
#define A2_PREFETCH(p) do {                        \
    _mm_prefetch((const char *) (p),        _MM_HINT_T0); \
    _mm_prefetch((const char *) (p) + 64,   _MM_HINT_T0); \
    _mm_prefetch((const char *) (p) + 128,  _MM_HINT_T0); \
    _mm_prefetch((const char *) (p) + 192,  _MM_HINT_T0); \
} while (0)

/* Same helpers the i8 kernels use. Duplicated rather than exported so
 * this file can be dropped without touching backend.c. */
static float a2_rd_f16p(const unsigned char *p) {
    return q_fp16_to_fp32((unsigned short) (p[0] | (p[1] << 8)));
}

/* Decode all EIGHT (scale, min) pairs of a k-quant superblock at once.
 *
 * This is word-parallel, not byte-at-a-time, and that matters more than
 * it looks: profiling the AVX2 Q4_K kernel showed the vector core
 * (loads, nibble split, VPMADDUBSW, VPMADDWD) costing 0.019 ns/weight
 * while the whole kernel cost 0.070 -- and **the scalar scale decode
 * alone was 0.038 of that**, twice the arithmetic it feeds. It runs
 * once per superblock per row, so on the 248320-row lm head it runs a
 * million times per token.
 *
 * The layout is six 6-bit scales and six 6-bit mins packed into 12
 * bytes, with the top two bits of the first four bytes carrying the
 * high nibbles of entries 4..7. Handling that per index needs a branch
 * and several shifts; handled as four 32-bit words it is three masks
 * and a shuffle, with no branch at all. Same bit layout, same output --
 * asserted against the per-index form over random inputs by
 * tests/t_scales.c.
 *
 * Measured: 9.76 ns -> 1.94 ns per superblock, a 5.0x cut on what was
 * the single largest term in the kernel. (Finding 53.)
 */
static void a2_scales8(const unsigned char *q, unsigned char *sc,
                       unsigned char *mn) {
    const unsigned int k1 = 0x3f3f3f3fu;   /* 6 bits per byte  */
    const unsigned int k2 = 0x0f0f0f0fu;   /* low nibble       */
    const unsigned int k3 = 0x03030303u;   /* top 2 bits       */
    unsigned int u[4], aux;

    memcpy(u, q, 12);
    u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
    aux  = u[1] & k1;
    u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
    u[2] = aux;
    u[0] &= k1;

    /* u[0],u[1] are the eight scales; u[2],u[3] the eight mins. */
    memcpy(sc,     &u[0], 4);
    memcpy(sc + 4, &u[1], 4);
    memcpy(mn,     &u[2], 4);
    memcpy(mn + 4, &u[3], 4);
}






/* ------------------------------------------------------------------ */
/* Q4_K                                                                */
/* ------------------------------------------------------------------ */

/* Horizontal sum of eight int32 lanes. Exact: integer. */
static int hsum8(__m256i v) {
    __m128i lo = _mm256_castsi256_si128(v);
    __m128i hi = _mm256_extracti128_si256(v, 1);
    __m128i s  = _mm_add_epi32(lo, hi);
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
}

/* Horizontal sum of one float vector. */
static float hsum_ps(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 hi = _mm256_extractf128_ps(v, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_add_ps(lo, _mm_movehl_ps(lo, lo));
    lo = _mm_add_ss(lo, _mm_shuffle_ps(lo, lo, 0x55));
    return _mm_cvtss_f32(lo);
}

/* The k-quant kernels below all use the SAME shape, which is the one
 * thing that separated this from llama.cpp's throughput:
 *
 *   - read activations from the Q8_K plane (xq->k8), which has ONE
 *     scale per 256 values. That is what makes the rest possible.
 *   - apply the weight's 6-bit scale IN THE INTEGER DOMAIN, folded
 *     into the VPMADDWD that had to widen int16->int32 anyway. Scaling
 *     costs nothing extra.
 *   - keep ONE int32 accumulator for the whole 256-weight superblock.
 *   - convert to float exactly once per superblock.
 *
 * The previous version reduced and converted every 32 weights: eight
 * horizontal sums and ~32 scalar float ops per superblock, which cost
 * more than the multiplies. This does one vector convert and one
 * multiply-add.
 *
 * Per-superblock int32 accumulation is also what keeps it SAFE.
 * Accumulating a whole 248320-column row in int32 would overflow;
 * per superblock the worst case is 66x inside the limit for Q6_K,
 * and tests/t_avx2sat.c checks all three formats.
 *
 * This is NOT bit-identical to the scalar path -- the sum is
 * reassociated and the scale is applied before rather than after the
 * float conversion. It is a different, equally valid rounding of the
 * same dot product, exactly as llama.cpp does it. tests/t_avx2acc.c
 * bounds the relative error instead of demanding equality.
 */

static float avx2_dot_q4_K(const unsigned char *b, const bk_qx *xq,
                           long ncols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    __m256 acc  = _mm256_setzero_ps();
    float  fsum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = a2_rd_f16p(b);
        const float dmin = a2_rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qs = b + 16;
        /* The weights are streamed once and never revisited, so the
         * hardware prefetcher has no history to work from at a row
         * boundary. Ask for the next superblock explicitly: in-model
         * this kernel ran 3.4x slower than the same code on a cached
         * working set, which is a memory-latency signature, not a
         * throughput one. */
        A2_PREFETCH(b + 144 * A2_PFDIST);
        const signed char *xk = xq->k8 + c;
        const float kd = xq->kd[c / 256];
        const int *ks  = xq->ks16 + (c / 16);
        __m256i sumi  = _mm256_setzero_si256();
        __m256i sumi2 = _mm256_setzero_si256();
        unsigned char SC[8], MN[8];
        int mini = 0;
        int j;

        a2_scales8(sc, SC, MN);

        for (j = 0; j < 4; j++) {
            unsigned char s1 = SC[j * 2], m1 = MN[j * 2];
            unsigned char s2 = SC[j * 2 + 1], m2 = MN[j * 2 + 1];
            __m256i qv, p;

            qv = _mm256_loadu_si256((const __m256i *) (qs + j * 32));

            /* TWO accumulators. Both halves of a 64-weight group used
             * to add into one `sumi`, serialising on a 1-cycle VPADDD
             * dependency chain 8 deep per superblock. Splitting them
             * lets the two chains issue in parallel; they are summed
             * once at the end of the block. Integer, so exact. */
            p = _mm256_madd_epi16(_mm256_set1_epi16((short) s1),
                    _mm256_maddubs_epi16(_mm256_and_si256(qv, m4),
                        _mm256_loadu_si256((const __m256i *) (xk + j * 64))));
            sumi = _mm256_add_epi32(sumi, p);

            p = _mm256_madd_epi16(_mm256_set1_epi16((short) s2),
                    _mm256_maddubs_epi16(
                        _mm256_and_si256(_mm256_srli_epi16(qv, 4), m4),
                        _mm256_loadu_si256((const __m256i *)
                                           (xk + j * 64 + 32))));
            sumi2 = _mm256_add_epi32(sumi2, p);

            /* The min term is also integer: min_j * sum(x over the
             * 32-block). Accumulating it as an int and converting once
             * per superblock removes 8 vcvtsi2ss + 24 scalar float ops
             * per block -- which the disassembly showed dominating the
             * loop once the multiplies were vectorised. */
            mini += (int) m1 * (ks[j * 4] + ks[j * 4 + 1]);
            mini += (int) m2 * (ks[j * 4 + 2] + ks[j * 4 + 3]);
        }

        acc = _mm256_add_ps(acc,
                  _mm256_mul_ps(_mm256_set1_ps(d * kd),
                      _mm256_cvtepi32_ps(_mm256_add_epi32(sumi, sumi2))));
        fsum -= dmin * kd * (float) mini;
        b += 144;
    }
    return hsum_ps(acc) + fsum;
}

/* ------------------------------------------------------------------ */
/* Q5_K -- as Q4_K plus a 1-bit high plane worth +16                   */
/* ------------------------------------------------------------------ */

static float avx2_dot_q5_K(const unsigned char *b, const bk_qx *xq,
                           long ncols) {
    const __m256i m4  = _mm256_set1_epi8(0x0F);
    const __m256i m1b = _mm256_set1_epi8(0x01);
    __m256 acc  = _mm256_setzero_ps();
    float  fsum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = a2_rd_f16p(b);
        const float dmin = a2_rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qh = b + 16;
        const unsigned char *qs = b + 48;
        A2_PREFETCH(b + 176 * A2_PFDIST);
        const signed char *xk = xq->k8 + c;
        const float kd = xq->kd[c / 256];
        const int *ks  = xq->ks16 + (c / 16);
        __m256i hv = _mm256_loadu_si256((const __m256i *) qh);
        __m256i sumi  = _mm256_setzero_si256();
        __m256i sumi2 = _mm256_setzero_si256();
        unsigned char SC[8], MN[8];
        int mini = 0;
        int j;

        a2_scales8(sc, SC, MN);

        for (j = 0; j < 4; j++) {
            unsigned char s1 = SC[j * 2], m1 = MN[j * 2];
            unsigned char s2 = SC[j * 2 + 1], m2 = MN[j * 2 + 1];
            __m256i qv, h1, h2, lo, hi, p;

            qv = _mm256_loadu_si256((const __m256i *) (qs + j * 32));

            /* the 5th bit, worth +16. Max weight 31, still unsigned
             * and still inside a byte, so VPMADDUBSW applies. */
            h1 = _mm256_slli_epi16(
                     _mm256_and_si256(_mm256_srli_epi16(hv, j * 2), m1b), 4);
            h2 = _mm256_slli_epi16(
                     _mm256_and_si256(_mm256_srli_epi16(hv, j * 2 + 1), m1b),
                     4);

            lo = _mm256_add_epi8(_mm256_and_si256(qv, m4), h1);
            hi = _mm256_add_epi8(
                     _mm256_and_si256(_mm256_srli_epi16(qv, 4), m4), h2);

            p = _mm256_madd_epi16(_mm256_set1_epi16((short) s1),
                    _mm256_maddubs_epi16(lo,
                        _mm256_loadu_si256((const __m256i *) (xk + j * 64))));
            sumi = _mm256_add_epi32(sumi, p);

            p = _mm256_madd_epi16(_mm256_set1_epi16((short) s2),
                    _mm256_maddubs_epi16(hi,
                        _mm256_loadu_si256((const __m256i *)
                                           (xk + j * 64 + 32))));
            sumi2 = _mm256_add_epi32(sumi2, p);

            mini += (int) m1 * (ks[j * 4] + ks[j * 4 + 1]);
            mini += (int) m2 * (ks[j * 4 + 2] + ks[j * 4 + 3]);
        }

        acc = _mm256_add_ps(acc,
                  _mm256_mul_ps(_mm256_set1_ps(d * kd),
                      _mm256_cvtepi32_ps(_mm256_add_epi32(sumi, sumi2))));
        fsum -= dmin * kd * (float) mini;
        b += 176;
    }
    return hsum_ps(acc) + fsum;
}

/* ------------------------------------------------------------------ */
/* Q6_K -- 4-bit low plane | 2-bit high plane, 16-weight scale groups  */
/* ------------------------------------------------------------------ */

static float avx2_dot_q6_K(const unsigned char *b, const bk_qx *xq,
                           long ncols) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const __m256i m3 = _mm256_set1_epi8(0x03);
    __m256 acc  = _mm256_setzero_ps();
    float  fsum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const unsigned char *ql = b;
        const unsigned char *qh = b + 128;
        const signed char   *sc = (const signed char *) (b + 192);
        const float d = a2_rd_f16p(b + 208);
        A2_PREFETCH(b + 210 * A2_PFDIST);
        const signed char *xk = xq->k8 + c;
        const float kd = xq->kd[c / 256];
        const int *ks  = xq->ks16 + (c / 16);
        __m256i sumi = _mm256_setzero_si256();
        int bias = 0;
        int n;

        for (n = 0; n < 2; n++) {
            const unsigned char *qln = ql + n * 64;
            const unsigned char *qhn = qh + n * 32;
            const signed char   *scn = sc + n * 8;
            const signed char   *xp  = xk + n * 128;
            __m256i lv0, lv1, hv, w, p, sv;
            __m256i sv01, sv23, sv45, sv67;
            int g = n * 8;

            /* Build the four scale vectors ONCE per half-block instead
             * of one _mm256_setr_epi16 per plane. Each is
             * [s(2k) x8 | s(2k+1) x8]: broadcast a 32-bit word holding
             * the pair, then a single permute puts one scale in each
             * 128-bit lane. Eight vpinsrw chains per superblock became
             * four broadcasts. */
            sv01 = _mm256_blend_epi32(_mm256_set1_epi16((short) scn[0]),
                                      _mm256_set1_epi16((short) scn[1]),
                                      0xF0);
            sv23 = _mm256_blend_epi32(_mm256_set1_epi16((short) scn[2]),
                                      _mm256_set1_epi16((short) scn[3]),
                                      0xF0);
            sv45 = _mm256_blend_epi32(_mm256_set1_epi16((short) scn[4]),
                                      _mm256_set1_epi16((short) scn[5]),
                                      0xF0);
            sv67 = _mm256_blend_epi32(_mm256_set1_epi16((short) scn[6]),
                                      _mm256_set1_epi16((short) scn[7]),
                                      0xF0);

            lv0 = _mm256_loadu_si256((const __m256i *) qln);
            lv1 = _mm256_loadu_si256((const __m256i *) (qln + 32));
            hv  = _mm256_loadu_si256((const __m256i *) qhn);

            /* Q6_K's scale changes every 16 weights, so unlike Q4_K/Q5_K
             * the multiplier is not a broadcast: each 32-weight plane
             * spans TWO scale groups. VPMADDUBSW pairs adjacent bytes,
             * so its 16 int16 outputs map 8-and-8 onto those two
             * groups -- build a vector that is scn[k] in the low half
             * and scn[k+1] in the high half and one VPMADDWD applies
             * both. */
#define Q6_PLANE(WEXPR, XOFF, SV)                                         \
            w  = (WEXPR);                                                 \
            sv = (SV);                                                    \
            p  = _mm256_madd_epi16(sv,                                    \
                     _mm256_maddubs_epi16(w,                              \
                         _mm256_loadu_si256((const __m256i *)             \
                                            (xp + (XOFF)))));             \
            sumi = _mm256_add_epi32(sumi, p);

            Q6_PLANE(_mm256_or_si256(_mm256_and_si256(lv0, m4),
                         _mm256_slli_epi16(_mm256_and_si256(hv, m3), 4)),
                     0, sv01)
            Q6_PLANE(_mm256_or_si256(_mm256_and_si256(lv1, m4),
                         _mm256_slli_epi16(
                             _mm256_and_si256(_mm256_srli_epi16(hv, 2), m3),
                             4)),
                     32, sv23)
            Q6_PLANE(_mm256_or_si256(
                         _mm256_and_si256(_mm256_srli_epi16(lv0, 4), m4),
                         _mm256_slli_epi16(
                             _mm256_and_si256(_mm256_srli_epi16(hv, 4), m3),
                             4)),
                     64, sv45)
            Q6_PLANE(_mm256_or_si256(
                         _mm256_and_si256(_mm256_srli_epi16(lv1, 4), m4),
                         _mm256_slli_epi16(
                             _mm256_and_si256(_mm256_srli_epi16(hv, 6), m3),
                             4)),
                     96, sv67)
#undef Q6_PLANE

            /* Q6_K stores q-32, so subtract 32*scale*sum(x) per group.
             * Scalar: eight multiply-adds per 128 weights. */
            bias += (int) scn[0] * ks[g + 0] + (int) scn[1] * ks[g + 1] +
                    (int) scn[2] * ks[g + 2] + (int) scn[3] * ks[g + 3] +
                    (int) scn[4] * ks[g + 4] + (int) scn[5] * ks[g + 5] +
                    (int) scn[6] * ks[g + 6] + (int) scn[7] * ks[g + 7];
        }

        acc = _mm256_add_ps(acc,
                  _mm256_mul_ps(_mm256_set1_ps(d * kd),
                                _mm256_cvtepi32_ps(sumi)));
        fsum -= d * kd * 32.0f * (float) bias;
        b += 210;
    }
    return hsum_ps(acc) + fsum;
}

/* ------------------------------------------------------------------ */
/* Q8_0 -- weights are SIGNED, so VPMADDUBSW does not apply.           */
/* VPMADDWD on widened int16 gives 16 MACs/instruction, which is       */
/* ample: Q8_0 is 0.1% of the forward pass.                            */
/* ------------------------------------------------------------------ */

static float avx2_dot_q8_0(const unsigned char *b, const bk_qx *xq,
                           long ncols) {
    float sum = 0.0f;
    long c;

    /* Q8_0 weights are SIGNED, so VPMADDUBSW does not apply -- its
     * first operand must be unsigned. VPMADDWD on sign-extended int16
     * gives 16 MACs per instruction, which is plenty: Q8_0 is 0.4% of
     * this model's forward pass.
     *
     * Two accumulators to break the dependency chain, and the float
     * scale is applied per 32-block because Q8_0 has no superblock to
     * amortise it over. */
    for (c = 0; c < ncols; c += 32) {
        const float d = a2_rd_f16p(b);
        const signed char *q  = (const signed char *) (b + 2);
        const short       *xp = xq->q + c;
        __m256i a1, a2;
        int a;

        a1 = _mm256_madd_epi16(
                 _mm256_cvtepi8_epi16(_mm_loadu_si128((const __m128i *) q)),
                 _mm256_loadu_si256((const __m256i *) xp));
        a2 = _mm256_madd_epi16(
                 _mm256_cvtepi8_epi16(
                     _mm_loadu_si128((const __m128i *) (q + 16))),
                 _mm256_loadu_si256((const __m256i *) (xp + 16)));

        a = hsum8(_mm256_add_epi32(a1, a2));
        sum += d * xq->d[c / 32] * (float) a;
        b += 34;
    }
    return sum;
}

/* Two rows at once.
 *
 * The activation plane is the same for every row of a matvec, so a
 * one-row kernel re-loads it nrows times and pays the loop overhead
 * nrows times. Doing two rows per pass halves both against the same
 * weight traffic, and gives the out-of-order engine two independent
 * dependency chains to interleave.
 *
 * This is where the remaining headroom was: the kernel measured
 * 0.102 ns/weight for every format regardless of bits-per-weight,
 * which is a uop-issue signature, not a bandwidth one (finding 49).
 */
static void avx2_dot2_q4_K(const unsigned char *b0, const unsigned char *b1,
                           const bk_qx *xq, long ncols,
                           float *o0, float *o1) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    float f0 = 0.0f, f1 = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d0 = a2_rd_f16p(b0), dm0 = a2_rd_f16p(b0 + 2);
        const float d1 = a2_rd_f16p(b1), dm1 = a2_rd_f16p(b1 + 2);
        const unsigned char *qs0 = b0 + 16, *qs1 = b1 + 16;
        const signed char *xk = xq->k8 + c;
        const float kd = xq->kd[c / 256];
        const int *ks  = xq->ks16 + (c / 16);
        __m256i s0 = _mm256_setzero_si256(), s1 = _mm256_setzero_si256();
        unsigned char SC0[8], MN0[8], SC1[8], MN1[8];
        int mi0 = 0, mi1 = 0;
        int j;

        A2_PREFETCH(b0 + 144 * A2_PFDIST);
        A2_PREFETCH(b1 + 144 * A2_PFDIST);
        a2_scales8(b0 + 4, SC0, MN0);
        a2_scales8(b1 + 4, SC1, MN1);

        for (j = 0; j < 4; j++) {
            __m256i xa = _mm256_loadu_si256((const __m256i *) (xk + j * 64));
            __m256i xb = _mm256_loadu_si256((const __m256i *)
                                            (xk + j * 64 + 32));
            __m256i q0 = _mm256_loadu_si256((const __m256i *) (qs0 + j * 32));
            __m256i q1 = _mm256_loadu_si256((const __m256i *) (qs1 + j * 32));

            s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC0[j * 2]),
                     _mm256_maddubs_epi16(_mm256_and_si256(q0, m4), xa)));
            s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC0[j * 2 + 1]),
                     _mm256_maddubs_epi16(
                         _mm256_and_si256(_mm256_srli_epi16(q0, 4), m4), xb)));

            s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC1[j * 2]),
                     _mm256_maddubs_epi16(_mm256_and_si256(q1, m4), xa)));
            s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC1[j * 2 + 1]),
                     _mm256_maddubs_epi16(
                         _mm256_and_si256(_mm256_srli_epi16(q1, 4), m4), xb)));

            mi0 += (int) MN0[j * 2]     * (ks[j * 4] + ks[j * 4 + 1]);
            mi0 += (int) MN0[j * 2 + 1] * (ks[j * 4 + 2] + ks[j * 4 + 3]);
            mi1 += (int) MN1[j * 2]     * (ks[j * 4] + ks[j * 4 + 1]);
            mi1 += (int) MN1[j * 2 + 1] * (ks[j * 4 + 2] + ks[j * 4 + 3]);
        }

        acc0 = _mm256_add_ps(acc0, _mm256_mul_ps(
                   _mm256_set1_ps(d0 * kd), _mm256_cvtepi32_ps(s0)));
        acc1 = _mm256_add_ps(acc1, _mm256_mul_ps(
                   _mm256_set1_ps(d1 * kd), _mm256_cvtepi32_ps(s1)));
        f0 -= dm0 * kd * (float) mi0;
        f1 -= dm1 * kd * (float) mi1;
        b0 += 144; b1 += 144;
    }
    *o0 = hsum_ps(acc0) + f0;
    *o1 = hsum_ps(acc1) + f1;
}

static void avx2_dot2_q5_K(const unsigned char *b0, const unsigned char *b1,
                           const bk_qx *xq, long ncols,
                           float *o0, float *o1) {
    const __m256i m4  = _mm256_set1_epi8(0x0F);
    const __m256i m1b = _mm256_set1_epi8(0x01);
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    float f0 = 0.0f, f1 = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d0 = a2_rd_f16p(b0), dm0 = a2_rd_f16p(b0 + 2);
        const float d1 = a2_rd_f16p(b1), dm1 = a2_rd_f16p(b1 + 2);
        const signed char *xk = xq->k8 + c;
        const float kd = xq->kd[c / 256];
        const int *ks  = xq->ks16 + (c / 16);
        __m256i h0 = _mm256_loadu_si256((const __m256i *) (b0 + 16));
        __m256i h1 = _mm256_loadu_si256((const __m256i *) (b1 + 16));
        __m256i s0 = _mm256_setzero_si256(), s1 = _mm256_setzero_si256();
        unsigned char SC0[8], MN0[8], SC1[8], MN1[8];
        int mi0 = 0, mi1 = 0;
        int j;

        A2_PREFETCH(b0 + 176 * A2_PFDIST);
        A2_PREFETCH(b1 + 176 * A2_PFDIST);
        a2_scales8(b0 + 4, SC0, MN0);
        a2_scales8(b1 + 4, SC1, MN1);

        for (j = 0; j < 4; j++) {
            __m256i xa = _mm256_loadu_si256((const __m256i *) (xk + j * 64));
            __m256i xb = _mm256_loadu_si256((const __m256i *)
                                            (xk + j * 64 + 32));
            __m256i q0 = _mm256_loadu_si256((const __m256i *)
                                            (b0 + 48 + j * 32));
            __m256i q1 = _mm256_loadu_si256((const __m256i *)
                                            (b1 + 48 + j * 32));
            __m256i a, bb;

            a  = _mm256_slli_epi16(_mm256_and_si256(
                     _mm256_srli_epi16(h0, j * 2), m1b), 4);
            bb = _mm256_slli_epi16(_mm256_and_si256(
                     _mm256_srli_epi16(h0, j * 2 + 1), m1b), 4);
            s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC0[j * 2]),
                     _mm256_maddubs_epi16(
                         _mm256_add_epi8(_mm256_and_si256(q0, m4), a), xa)));
            s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC0[j * 2 + 1]),
                     _mm256_maddubs_epi16(_mm256_add_epi8(
                         _mm256_and_si256(_mm256_srli_epi16(q0, 4), m4), bb),
                         xb)));

            a  = _mm256_slli_epi16(_mm256_and_si256(
                     _mm256_srli_epi16(h1, j * 2), m1b), 4);
            bb = _mm256_slli_epi16(_mm256_and_si256(
                     _mm256_srli_epi16(h1, j * 2 + 1), m1b), 4);
            s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC1[j * 2]),
                     _mm256_maddubs_epi16(
                         _mm256_add_epi8(_mm256_and_si256(q1, m4), a), xa)));
            s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(
                     _mm256_set1_epi16((short) SC1[j * 2 + 1]),
                     _mm256_maddubs_epi16(_mm256_add_epi8(
                         _mm256_and_si256(_mm256_srli_epi16(q1, 4), m4), bb),
                         xb)));

            mi0 += (int) MN0[j * 2]     * (ks[j * 4] + ks[j * 4 + 1]);
            mi0 += (int) MN0[j * 2 + 1] * (ks[j * 4 + 2] + ks[j * 4 + 3]);
            mi1 += (int) MN1[j * 2]     * (ks[j * 4] + ks[j * 4 + 1]);
            mi1 += (int) MN1[j * 2 + 1] * (ks[j * 4 + 2] + ks[j * 4 + 3]);
        }

        acc0 = _mm256_add_ps(acc0, _mm256_mul_ps(
                   _mm256_set1_ps(d0 * kd), _mm256_cvtepi32_ps(s0)));
        acc1 = _mm256_add_ps(acc1, _mm256_mul_ps(
                   _mm256_set1_ps(d1 * kd), _mm256_cvtepi32_ps(s1)));
        f0 -= dm0 * kd * (float) mi0;
        f1 -= dm1 * kd * (float) mi1;
        b0 += 176; b1 += 176;
    }
    *o0 = hsum_ps(acc0) + f0;
    *o1 = hsum_ps(acc1) + f1;
}

static void avx2_dot2_q6_K(const unsigned char *b0, const unsigned char *b1,
                           const bk_qx *xq, long ncols,
                           float *o0, float *o1) {
    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const __m256i m3 = _mm256_set1_epi8(0x03);
    __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
    float f0 = 0.0f, f1 = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d0 = a2_rd_f16p(b0 + 208), d1 = a2_rd_f16p(b1 + 208);
        const signed char *xk = xq->k8 + c;
        const float kd = xq->kd[c / 256];
        const int *ks  = xq->ks16 + (c / 16);
        __m256i s0 = _mm256_setzero_si256(), s1 = _mm256_setzero_si256();
        int bi0 = 0, bi1 = 0;
        int n;

        A2_PREFETCH(b0 + 210 * A2_PFDIST);
        A2_PREFETCH(b1 + 210 * A2_PFDIST);

        for (n = 0; n < 2; n++) {
            const signed char *sc0 = (const signed char *) (b0 + 192 + n * 8);
            const signed char *sc1 = (const signed char *) (b1 + 192 + n * 8);
            const unsigned char *l0 = b0 + n * 64, *h0p = b0 + 128 + n * 32;
            const unsigned char *l1 = b1 + n * 64, *h1p = b1 + 128 + n * 32;
            const signed char *xp = xk + n * 128;
            __m256i L00 = _mm256_loadu_si256((const __m256i *) l0);
            __m256i L01 = _mm256_loadu_si256((const __m256i *) (l0 + 32));
            __m256i H0  = _mm256_loadu_si256((const __m256i *) h0p);
            __m256i L10 = _mm256_loadu_si256((const __m256i *) l1);
            __m256i L11 = _mm256_loadu_si256((const __m256i *) (l1 + 32));
            __m256i H1  = _mm256_loadu_si256((const __m256i *) h1p);
            int g = n * 8, t;

#define Q6_2(LA, HA, SH, LB, HB, XOFF, K)                                  \
            {                                                              \
                __m256i xv = _mm256_loadu_si256((const __m256i *)          \
                                                (xp + (XOFF)));            \
                __m256i sa = _mm256_blend_epi32(                           \
                        _mm256_set1_epi16((short) sc0[(K)]),               \
                        _mm256_set1_epi16((short) sc0[(K) + 1]), 0xF0);    \
                __m256i sb = _mm256_blend_epi32(                           \
                        _mm256_set1_epi16((short) sc1[(K)]),               \
                        _mm256_set1_epi16((short) sc1[(K) + 1]), 0xF0);    \
                s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(sa,            \
                        _mm256_maddubs_epi16(_mm256_or_si256(              \
                            _mm256_and_si256((LA), m4),                    \
                            _mm256_slli_epi16(_mm256_and_si256(            \
                                _mm256_srli_epi16((HA), (SH)), m3), 4)),   \
                            xv)));                                         \
                s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(sb,            \
                        _mm256_maddubs_epi16(_mm256_or_si256(              \
                            _mm256_and_si256((LB), m4),                    \
                            _mm256_slli_epi16(_mm256_and_si256(            \
                                _mm256_srli_epi16((HB), (SH)), m3), 4)),   \
                            xv)));                                         \
            }

            Q6_2(L00, H0, 0, L10, H1, 0, 0)
            Q6_2(L01, H0, 2, L11, H1, 32, 2)
            Q6_2(_mm256_srli_epi16(L00, 4), H0, 4,
                 _mm256_srli_epi16(L10, 4), H1, 64, 4)
            Q6_2(_mm256_srli_epi16(L01, 4), H0, 6,
                 _mm256_srli_epi16(L11, 4), H1, 96, 6)
#undef Q6_2

            for (t = 0; t < 8; t++) {
                bi0 += (int) sc0[t] * ks[g + t];
                bi1 += (int) sc1[t] * ks[g + t];
            }
        }

        acc0 = _mm256_add_ps(acc0, _mm256_mul_ps(
                   _mm256_set1_ps(d0 * kd), _mm256_cvtepi32_ps(s0)));
        acc1 = _mm256_add_ps(acc1, _mm256_mul_ps(
                   _mm256_set1_ps(d1 * kd), _mm256_cvtepi32_ps(s1)));
        f0 -= d0 * kd * 32.0f * (float) bi0;
        f1 -= d1 * kd * 32.0f * (float) bi1;
        b0 += 210; b1 += 210;
    }
    *o0 = hsum_ps(acc0) + f0;
    *o1 = hsum_ps(acc1) + f1;
}


/* ------------------------------------------------------------------ */
/* Helpers hoisted out of the i486-baseline translation units.         */
/*                                                                     */
/* backend.c and qwen35.c are compiled at the i486 baseline so that a  */
/* SINGLE binary runs on a 486 and still accelerates on a modern CPU.  */
/* They therefore cannot contain AVX2 themselves. These wrappers live  */
/* here -- the only object built with -mavx2 -- and each returns 0     */
/* when AVX2 is not usable, so the caller runs its scalar loop.        */
/*                                                                     */
/* The gate is bk_avx2_available(): CPUID *and* XGETBV, because an XP  */
/* box on Haswell silicon reports AVX2 and cannot execute it.          */
/* ------------------------------------------------------------------ */

/* max |x[i]| over n floats. */
int bk_a2_amax(const float *x, long n, float *out) {
    __m256 vm, sign;
    float tmp[8], amax = 0.0f;
    long i = 0;
    int u;

    if (!bk_avx2_available()) return 0;

    vm   = _mm256_setzero_ps();
    sign = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    for (; i + 7 < n; i += 8)
        vm = _mm256_max_ps(vm, _mm256_and_ps(_mm256_loadu_ps(x + i), sign));
    _mm256_storeu_ps(tmp, vm);
    for (u = 0; u < 8; u++) if (tmp[u] > amax) amax = tmp[u];
    for (; i < n; i++) {
        float a = x[i] < 0.0f ? -x[i] : x[i];
        if (a > amax) amax = a;
    }
    *out = amax;
    return 1;
}

/* Quantise up to 256 activations to int8 with scale `id`, and
 * accumulate per-16 sums. Rounding must match the scalar path exactly:
 * round half AWAY FROM ZERO. _mm256_cvtps_epi32 rounds half to even,
 * so the +/-0.5 bias is added and truncation used instead. */
int bk_a2_quant256(const float *x, long n, float id,
                   signed char *q8, int *s16) {
    __m256  vid, vhalf, vzero;
    __m256i vmax, vmin;
    long i = 0;

    if (!bk_avx2_available()) return 0;

    vid   = _mm256_set1_ps(id);
    vhalf = _mm256_set1_ps(0.5f);
    vzero = _mm256_setzero_ps();
    vmax  = _mm256_set1_epi32(127);
    vmin  = _mm256_set1_epi32(-127);

    for (; i + 7 < n; i += 8) {
        __m256 v = _mm256_mul_ps(_mm256_loadu_ps(x + i), vid);
        __m256 bias = _mm256_blendv_ps(_mm256_sub_ps(vzero, vhalf), vhalf,
                          _mm256_cmp_ps(v, vzero, _CMP_GE_OQ));
        __m256i qi = _mm256_cvttps_epi32(_mm256_add_ps(v, bias));
        int tmp[8];
        int u;
        qi = _mm256_min_epi32(_mm256_max_epi32(qi, vmin), vmax);
        _mm256_storeu_si256((__m256i *) tmp, qi);
        for (u = 0; u < 8; u++) {
            q8[i + u] = (signed char) tmp[u];
            s16[(i + u) / 16] += tmp[u];
        }
    }
    for (; i < n; i++) {
        float v = x[i] * id;
        int   c = (int) (v + (v >= 0.0f ? 0.5f : -0.5f));
        if (c >  127) c =  127;
        if (c < -127) c = -127;
        q8[i] = (signed char) c;
        s16[i / 16] += c;
    }
    return 1;
}

/* Gated DeltaNet recurrence for one value head.
 *
 * Two fused "update the row, then dot it" passes over a hd x hd state
 * matrix. GCC will not vectorise a read-modify-write feeding a
 * reduction (verified: zero ymm emitted), and this was 10.9% of the
 * forward pass, so it is written out. */
int bk_a2_dn_recur(float *S, const float *k, const float *q, const float *v,
                   float *kvd, float *o, int hd,
                   float decay, float beta, float scale) {
    __m256 vdecay;
    int i, j;

    if (!bk_avx2_available()) return 0;

    vdecay = _mm256_set1_ps(decay);
    for (j = 0; j < hd; j++) {
        float *row = S + (size_t) j * hd;
        __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
        float s;
        for (i = 0; i + 15 < hd; i += 16) {
            __m256 e0 = _mm256_mul_ps(_mm256_loadu_ps(row + i), vdecay);
            __m256 e1 = _mm256_mul_ps(_mm256_loadu_ps(row + i + 8), vdecay);
            _mm256_storeu_ps(row + i,     e0);
            _mm256_storeu_ps(row + i + 8, e1);
            a0 = _mm256_add_ps(a0, _mm256_mul_ps(e0, _mm256_loadu_ps(k + i)));
            a1 = _mm256_add_ps(a1,
                     _mm256_mul_ps(e1, _mm256_loadu_ps(k + i + 8)));
        }
        s = hsum_ps(_mm256_add_ps(a0, a1));
        for (; i < hd; i++) {
            float e = row[i] * decay;
            row[i] = e;
            s += e * k[i];
        }
        kvd[j] = (v[j] - s) * beta;
    }

    for (j = 0; j < hd; j++) {
        float *row = S + (size_t) j * hd;
        __m256 vd = _mm256_set1_ps(kvd[j]);
        __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
        float s;
        for (i = 0; i + 15 < hd; i += 16) {
            __m256 e0 = _mm256_add_ps(_mm256_loadu_ps(row + i),
                            _mm256_mul_ps(vd, _mm256_loadu_ps(k + i)));
            __m256 e1 = _mm256_add_ps(_mm256_loadu_ps(row + i + 8),
                            _mm256_mul_ps(vd, _mm256_loadu_ps(k + i + 8)));
            _mm256_storeu_ps(row + i,     e0);
            _mm256_storeu_ps(row + i + 8, e1);
            a0 = _mm256_add_ps(a0, _mm256_mul_ps(e0, _mm256_loadu_ps(q + i)));
            a1 = _mm256_add_ps(a1,
                     _mm256_mul_ps(e1, _mm256_loadu_ps(q + i + 8)));
        }
        s = hsum_ps(_mm256_add_ps(a0, a1));
        for (; i < hd; i++) {
            float e = row[i] + kvd[j] * k[i];
            row[i] = e;
            s += e * q[i];
        }
        o[j] = s * scale;
    }
    return 1;
}

/* ------------------------------------------------------------------ */

void qmv_avx2(int type, const unsigned char *w, const float *x, float *y,
              long ncols, long nrows) {
    const bk_qx *xq;
    long rowbytes, r;

    if (type == GGML_TYPE_F32 || type == GGML_TYPE_F16 ||
        (ncols % 32) != 0) {
        qmv_ref(type, w, x, y, ncols, nrows);
        return;
    }

    /* Formats this kernel does not implement fall through to the
     * portable integer path, not to the float reference. */
    if (type != GGML_TYPE_Q4_K && type != GGML_TYPE_Q5_K &&
        type != GGML_TYPE_Q6_K && type != GGML_TYPE_Q8_0) {
        qmv_i8(type, w, x, y, ncols, nrows);
        return;
    }

    /* Build ONLY the plane this format needs.
     *
     * The k-quant kernels read the Q8_K-style plane (k8/kd/ks16); they
     * never touch the per-32 plane (q/d/s/s16). Building both on every
     * call meant two full passes over x for ~187 matvecs per forward
     * pass, and the per-32 plane was pure waste for 99.7% of them. */
    if (type == GGML_TYPE_Q8_0) xq = bk_quantize_x(x, ncols);
    else                        xq = bk_quantize_k(x, ncols);
    rowbytes = gg_type_size(type, ncols);

    switch (type) {
        case GGML_TYPE_Q4_K:
            for (r = 0; r + 1 < nrows; r += 2)
                avx2_dot2_q4_K(w + r * rowbytes, w + (r + 1) * rowbytes,
                               xq, ncols, &y[r], &y[r + 1]);
            if (r < nrows)
                y[r] = avx2_dot_q4_K(w + r * rowbytes, xq, ncols);
            break;
        case GGML_TYPE_Q5_K:
            for (r = 0; r + 1 < nrows; r += 2)
                avx2_dot2_q5_K(w + r * rowbytes, w + (r + 1) * rowbytes,
                               xq, ncols, &y[r], &y[r + 1]);
            if (r < nrows)
                y[r] = avx2_dot_q5_K(w + r * rowbytes, xq, ncols);
            break;
        case GGML_TYPE_Q6_K:
            for (r = 0; r + 1 < nrows; r += 2)
                avx2_dot2_q6_K(w + r * rowbytes, w + (r + 1) * rowbytes,
                               xq, ncols, &y[r], &y[r + 1]);
            if (r < nrows)
                y[r] = avx2_dot_q6_K(w + r * rowbytes, xq, ncols);
            break;
        default: /* GGML_TYPE_Q8_0 */
            for (r = 0; r < nrows; r++)
                y[r] = avx2_dot_q8_0(w + r * rowbytes, xq, ncols);
            break;
    }
    /* No VZEROUPPER needed: GCC emits it at every function boundary
     * that mixes encodings, and nothing here calls into x87/MMX. */
}

/* Run-time gate. AVX2 needs BOTH the CPU feature bit and OS support
 * for saving ymm across a context switch -- Windows XP has the former
 * on late silicon and never the latter, which is exactly the case
 * that faults rather than degrades. */
int bk_avx2_available(void) {
    return bk_cpu_has_avx2();
}

#elif defined(INFER_HAVE_AVX2)
/* INFER_HAVE_AVX2 set but this object was not compiled with -mavx2.
 * Provide a never-selected backend entry; the bk_a2_* helpers come
 * from backend_a2stub.c. */
void qmv_avx2(int type, const unsigned char *w, const float *x, float *y,
              long ncols, long nrows) {
    qmv_i8(type, w, x, y, ncols, nrows);
}
int bk_avx2_available(void) { return 0; }

#endif /* INFER_HAVE_AVX2 && __AVX2__ */
