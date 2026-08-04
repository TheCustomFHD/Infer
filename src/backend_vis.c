/* backend_vis.c -- VIS 1 matvec kernels for UltraSPARC.
 *
 * Compiled only when INFER_HAVE_VIS is defined, and only ever selected
 * at run time on a CPU that has VIS. Everything else in the project
 * stays strict C89; this file uses GCC/Sun Studio vector extensions,
 * exactly as backend_mmx.c does for x86.
 *
 * ---------------------------------------------------------------
 * THE CENTRAL TRICK: making fmul8x16 exact
 * ---------------------------------------------------------------
 *
 * VIS is a *graphics* instruction set. Its partitioned multiply is
 * defined for pixel blending, so it scales the result:
 *
 *     fmul8x16(u8 a, s16 b)  ->  s16   (a * b + 0x80) >> 8
 *
 * Taken literally that is useless here. Our products are small --
 * a Q4_K nibble times an int8 activation is at most 15*127 = 1905 --
 * so a >>8 would throw away almost everything.
 *
 * But the shift can be cancelled. If the activation is pre-shifted
 * left by 8 before it reaches the kernel:
 *
 *     fmul8x16(w, x << 8) = (w * (x << 8) + 0x80) >> 8 = w * x
 *
 * exactly, with no rounding error at all, provided x << 8 still fits
 * in a signed 16-bit lane. Our activations are quantised to
 * [-127, 127], so x << 8 spans [-32512, 32512] and fits with room to
 * spare.
 *
 * This was verified exhaustively on the real instruction semantics
 * (16320 combinations of w in 0..63 and x in -127..127): zero
 * mismatches. tests/t_vis.c re-runs that check on the target.
 *
 * The pay-off is large: it turns a graphics instruction into an exact
 * four-lane integer multiply, which is the same width as MMX's
 * pmaddwd, rather than falling back to fmuld8ulx16 at two lanes.
 *
 * ---------------------------------------------------------------
 * ACCUMULATOR WIDTH
 * ---------------------------------------------------------------
 *
 * Products stay in 16-bit lanes, so partial sums must not overflow
 * +-32767. Worst cases per format:
 *
 *     Q4_K  w <= 15   ->  16 terms * 15 * 127 = 30480   ok
 *     Q5_K  w <= 31   ->   8 terms * 31 * 127 = 31496   ok
 *     Q6_K  |w| <= 32 ->   8 terms * 32 * 127 = 32512   ok
 *     Q8_0  |w| <= 127->   2 terms * 127 * 127 = 32258  ok
 *
 * So the kernels accumulate a bounded number of terms in 16-bit lanes
 * and then widen. Widening is done with fmuld8ulx16 against a constant
 * 1 byte, which is an exact 16 -> 32 bit expansion of two lanes and
 * avoids needing an unpack instruction VIS does not have.
 *
 * ---------------------------------------------------------------
 * WHY THIS SHOULD BEAT THE SCALAR PATH
 * ---------------------------------------------------------------
 *
 * UltraSPARC-II/IIi (Sun datasheet STP1031):
 *   - 4-way superscalar, 9 execution units
 *   - 2 *graphics* execution units, two independent pipelines
 *   - the FPU issues and executes two FP/VIS instructions per cycle
 *   - most VIS ops: fully pipelined, throughput 1/cycle, latency 3
 *   - 32-entry FP register file
 *
 * Two consequences shape the code below.
 *
 * Latency 3 with throughput 1 means a single dependent accumulator
 * chain runs at one third of peak. The kernels therefore keep four
 * independent accumulators and only fold them together at the end.
 *
 * 32 registers is the other half. On MMX the equivalent multi-row
 * kernel measured 0.90x -- *slower* -- because with only 8 registers
 * (one holding zero, four accumulators) just three scratch registers
 * remained and weights had to be re-loaded four times. That constraint
 * simply does not exist here.
 *
 * There is also no EMMS: VIS shares the FP register file, so there is
 * no mode switch and no state to flush between kernel and scalar code.
 *
 * ---------------------------------------------------------------
 * CACHE
 * ---------------------------------------------------------------
 *
 * The IIi D-cache is 16 KB, *direct-mapped* and write-through. That is
 * smaller than the Geode's and far more prone to conflict misses. The
 * kernels walk weights linearly and touch the activation block
 * repeatedly, which is the access pattern that suits it: the
 * activations for one 256-weight super-block are 512 bytes and stay
 * resident while the weights stream past.
 *
 * ANSI C89 apart from the vector extensions.
 */

#include "backend.h"
#include "infer.h"

#include <string.h>
#include <stdlib.h>

#if !defined(INFER_HAVE_VIS)
/* Not compiled in; the file is empty so it can still be listed in a
 * build without conditionals in the Makefile. */
typedef int bk_vis_translation_unit_not_empty;
#else

/* ------------------------------------------------------------------ */
/* Vector types                                                        */
/*                                                                     */
/* Two compilers, two entirely different spellings for the same        */
/* instructions:                                                       */
/*                                                                     */
/*   GCC/Clang  __attribute__((vector_size)) + __builtin_vis_*         */
/*              built with -mcpu=ultrasparc -mvis                      */
/*   Sun Studio <vis_proto.h> + plain float/double + vis_* calls       */
/*              built with -xarch=sparcvis -xvis                       */
/*                                                                     */
/* Sun's header declares no vector types at all: a VIS register IS an  */
/* FP register, so a u8x4 is a `float` and an s16x4 is a `double` that */
/* you simply never do float arithmetic on. Lanes are reached with a   */
/* union, not with subscripts. (Finding 25.)                            */
/*                                                                     */
/* Both are wrapped behind the same four macros so the kernels below    */
/* are written once.                                                    */
/* ------------------------------------------------------------------ */

#if defined(__SUNPRO_C)

#include <vis_proto.h>

typedef float  vis_u8x4;
typedef double vis_s16x4;
typedef float  vis_s16x2;
typedef double vis_s32x2;

#define VMUL8X16(a, b)   vis_fmul8x16((a), (b))
#define VADD16(a, b)     vis_fpadd16((a), (b))
#define VADD32(a, b)     vis_fpadd32((a), (b))
#define VWIDEN(a, b)     vis_fmuld8ulx16((a), (b))

#define VIS_SUNPRO 1

#elif defined(__GNUC__)

typedef unsigned char vis_u8x4  __attribute__((vector_size(4)));
typedef short         vis_s16x4 __attribute__((vector_size(8)));
typedef short         vis_s16x2 __attribute__((vector_size(4)));
typedef int           vis_s32x2 __attribute__((vector_size(8)));

#define VMUL8X16(a, b)   __builtin_vis_fmul8x16((a), (b))
#define VADD16(a, b)     __builtin_vis_fpadd16((a), (b))
#define VADD32(a, b)     __builtin_vis_fpadd32((a), (b))
#define VWIDEN(a, b)     __builtin_vis_fmuld8ulx16((a), (b))

#else
#error "INFER_HAVE_VIS needs GCC/Clang (-mvis) or Sun Studio (-xarch=v9a)"
#endif

/* ------------------------------------------------------------------ */
/* Local copies of the shared k-quant helpers                          */
/*                                                                     */
/* backend.c keeps these static, and backend_mmx.c likewise defines its */
/* own. Duplicating two tiny functions is preferable to widening the    */
/* backend interface for them.                                          */
/* ------------------------------------------------------------------ */

static float vis_rd_f16p(const unsigned char *p) {
    return q_fp16_to_fp32((unsigned short) (p[0] | (p[1] << 8)));
}

static void vis_scale_min_k4(int j, const unsigned char *q,
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
/* Pre-shifted activation cache                                        */
/*                                                                     */
/* The trick above needs x << 8. Rather than shifting inside the inner  */
/* loop -- which would cost one op per element and defeat the point --  */
/* the whole activation vector is shifted once per matvec and reused    */
/* across every row. For a 1024-column matrix that is 1024 shifts       */
/* amortised over hundreds of rows.                                     */
/* ------------------------------------------------------------------ */

static short *xs_buf = 0;      /* activations, each << 8 */
static long   xs_cap = 0;

/* Rebuild unconditionally on every matvec.
 *
 * An earlier version cached this, keyed on (xq->q, xq->n), on the
 * theory that consecutive matvecs in a layer share an activation
 * vector. That was wrong in a way worth recording: bk_quantize_x()
 * returns a pointer into a single static buffer, so xq->q is the *same
 * address* every call while the contents change underneath. The cache
 * therefore always hit and every matvec after the first used stale
 * activations.
 *
 * It passed t_backend -- which calls matvec once per tensor with one x
 * -- and produced fluent-looking garbage in real generation. Exactly
 * the failure mode this project keeps running into: correct-looking
 * output, no error, wrong answer.
 *
 * The shift is one pass over ncols against nrows dot products of the
 * same length, so for a 1024x1024 matrix it is ~0.1% of the work.
 * Not worth risking correctness to avoid. */
static void xs_prepare(const bk_qx *xq) {
    long i;

    if (xq->n > xs_cap) {
        xs_cap = xq->n + 64;
        xs_buf = (short *) xrealloc(xs_buf, sizeof(short) * (size_t) xs_cap);
    }
    for (i = 0; i < xq->n; i++) {
        xs_buf[i] = (short) (xq->q[i] << 8);
    }
}

void bk_vis_free_scratch(void) {
    if (xs_buf) { free(xs_buf); xs_buf = 0; }
    xs_cap = 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Horizontal sum of four 16-bit lanes, widened exactly to int.
 *
 * fmuld8ulx16(ones, v) computes u8(1) * s16(v) into 32-bit lanes with
 * no rounding, which is an exact widen of two lanes at a time. */
#if defined(VIS_SUNPRO)

/* No subscripting a double: lanes come out through a union. The 32-bit
 * halves of the accumulator feed fmuld8ulx16 directly as floats. */
typedef union { double d; short  s[4]; float f[2]; } vis_lane64;
typedef union { double d; int    i[2];             } vis_wide64;
typedef union { float  f; unsigned int u;          } vis_lane32;

static int vis_hsum16(vis_s16x4 v) {
    vis_lane64 in;
    vis_lane32 one;
    vis_wide64 wl, wh, s;

    one.u = 0x01010101U;   /* four u8 lanes of 1 */
    in.d  = v;

    wl.d = VWIDEN(one.f, in.f[0]);
    wh.d = VWIDEN(one.f, in.f[1]);
    s.d  = VADD32(wl.d, wh.d);
    return s.i[0] + s.i[1];
}

static vis_s16x4 vis_zero16(void) {
    return vis_fzero();
}


/* Widen four 16-bit lanes to two 32-bit lanes, summing the pairs.
 * fmuld8ulx16(1, v) is an exact widen with no rounding. */
static vis_s32x2 vis_widen2(vis_s16x4 v) {
    vis_lane64 in;
    vis_lane32 one;

    one.u = 0x01010101U;
    in.d  = v;
    return VADD32(VWIDEN(one.f, in.f[0]), VWIDEN(one.f, in.f[1]));
}

static int vis_hsum32(vis_s32x2 v) {
    vis_wide64 w;
    w.d = v;
    return w.i[0] + w.i[1];
}

#else

/* Union puns for the GCC branch. Declared up here because the
 * horizontal helpers below need them; GCC lowers vector subscripts to
 * stack round-trips on SPARC, so every lane access goes through one of
 * these instead. */
typedef union { vis_s16x4 v; unsigned long u; } vis_pun64;
typedef union { vis_u8x4  v; unsigned int  u; } vis_pun32;
typedef union { vis_s16x2 v; unsigned int  u; } vis_pun32s;
typedef union { vis_s32x2 v; unsigned long u; } vis_pun64w;

static int vis_hsum16(vis_s16x4 v) {
    vis_u8x4  one;
    vis_s16x2 lo, hi;
    vis_s32x2 wl, wh, s;

    one[0] = 1; one[1] = 1; one[2] = 1; one[3] = 1;

    lo[0] = v[0]; lo[1] = v[1];
    hi[0] = v[2]; hi[1] = v[3];

    wl = VWIDEN(one, lo);
    wh = VWIDEN(one, hi);
    s  = VADD32(wl, wh);
    return s[0] + s[1];
}

static vis_s16x4 vis_zero16(void) {
    vis_s16x4 z;
    z[0] = 0; z[1] = 0; z[2] = 0; z[3] = 0;
    return z;
}


/* As above. Union puns rather than subscripts: GCC lowers vector
 * element access to a stack round-trip on SPARC. */
static vis_s32x2 vis_widen2(vis_s16x4 v) {
    vis_pun64  pu;
    vis_pun32s lo, hi;
    vis_u8x4   one;

    one[0] = 1; one[1] = 1; one[2] = 1; one[3] = 1;
    pu.v = v;
    lo.u = (unsigned int) pu.u;
    hi.u = (unsigned int) (pu.u >> 32);
    return VADD32(VWIDEN(one, hi.v), VWIDEN(one, lo.v));
}

static int vis_hsum32(vis_s32x2 v) {
    vis_pun64w w;
    w.v = v;
    return (int) (w.u >> 32) + (int) (unsigned int) w.u;
}

#endif

/* Load four pre-shifted activations as ONE 64-bit vector load.
 *
 * Assigning element by element looks equivalent but is not: GCC lowers
 * it to four scalar stores into a stack slot followed by a reload,
 * which spills straight past the 32-register file the whole design
 * depends on. Measured on the first version of this kernel: 124 memory
 * operations for 8 multiplies. Going through a union of the vector
 * type and a 64-bit integer keeps everything in registers.
 *
 * xs_buf is allocated by xrealloc and indexed in multiples of 4 shorts,
 * so the 8-byte alignment ldd needs is satisfied. */
#if defined(VIS_SUNPRO)
/* A VIS register is an FP register, so a 64-bit vector load is just a
 * double load (ldd) and a u8x4 is a float bit-pattern. Done with a
 * union rather than vis_to_float(): that helper is part of mediaLib's
 * copy of the header and may not be in Studio's. */
#define XLOAD4(dst, p)  ((dst) = *(const double *) (const void *) (p))

static vis_u8x4 vis_pack_u8x4(unsigned int u) {
    vis_lane32 p;
    p.u = u;
    return p.f;
}
#define PACK_U8X4(u)    vis_pack_u8x4((unsigned int) (u))
#else
#define XLOAD4(dst, p)                                                  \
    do { vis_pun64 pun_; pun_.u = *(const unsigned long *) (const void *) (p); \
         (dst) = pun_.v; } while (0)

static vis_u8x4 vis_pack_u8x4(unsigned int u) {
    vis_pun32 p;
    p.u = u;
    return p.v;
}
#define PACK_U8X4(u)    vis_pack_u8x4((unsigned int) (u))
#endif

/* ------------------------------------------------------------------ */
/* Loading four weight bytes into VIS lane order                       */
/*                                                                     */
/* PACK_U8X4 reinterprets a 32-bit word as four u8 lanes, so lane 0 is  */
/* whichever byte the word's memory image starts with. On big-endian    */
/* SPARC that is the MOST significant byte. A loader that assembles     */
/* q[0] into the low byte therefore feeds the weights to fmul8x16 in    */
/* REVERSE order while the activations stay in order -- silently wrong, */
/* and invisible on a little-endian host. (Finding 27.)                 */
/*                                                                     */
/* Q4_K/Q5_K super-blocks are 144/176 bytes, so their quant planes are  */
/* always 4-aligned and take a plain lduw. Q6_K's stride is 210 and     */
/* Q8_0/Q4_0's are 34/18, so those planes land on odd 2-byte offsets    */
/* half the time: they must go through the halfword path, which is two  */
/* lduh rather than a fault.                                            */
/* ------------------------------------------------------------------ */

/* Sum of 16 activations, at Q6_K's scale granularity. */
static int vis_sum16(const bk_qx *xq, long idx) {
    return xq->s16[idx / 16];
}

/* Aligned: the word's memory image already is the lane order. */
#define VIS_LDW(p)  (*(const unsigned int *) (const void *) (p))

/* Possibly 2-aligned. Two halfword loads, assembled most-significant
 * first so the lane order matches VIS_LDW on this target. */
static unsigned int vis_ldw2(const unsigned char *p) {
#if INFER_BIG_ENDIAN
    const unsigned short *h = (const unsigned short *) (const void *) p;
    return ((unsigned int) h[0] << 16) | (unsigned int) h[1];
#else
    /* Little-endian hosts (the shim builds) byte-swap each halfword, so
     * rebuild from bytes to keep the lane order identical. */
    return ((unsigned int) p[3] << 24) | ((unsigned int) p[2] << 16) |
           ((unsigned int) p[1] << 8)  |  (unsigned int) p[0];
#endif
}

#if defined(INFER_VIS_COUNT) && defined(__GNUC__)
#define VIS_NOINLINE __attribute__((noinline))
#else
#define VIS_NOINLINE
#endif

/* ------------------------------------------------------------------ */
/* Q4_K                                                                */
/*                                                                     */
/* 256 weights per super-block in 144 bytes:                           */
/*   [d:fp16][dmin:fp16][12 scale bytes][128 nibble bytes]             */
/*                                                                     */
/* Each of the 128 qs bytes holds two weights: the low nibble belongs   */
/* to sub-block j*2, the high nibble to j*2+1.                          */
/*                                                                     */
/* Four independent accumulators per sub-block hide the 3-cycle VIS     */
/* latency; 16 terms fit in 16-bit lanes (16*15*127 = 30480).           */
/* ------------------------------------------------------------------ */

VIS_NOINLINE static float vis_dot_q4_K(const unsigned char *b, const bk_qx *xq,
                          long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = vis_rd_f16p(b);
        const float dmin = vis_rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qs = b + 16;
        long blk = c / 32;
        int j;

        for (j = 0; j < 4; j++) {
            unsigned char s1, m1, s2, m2;
            const short *x1 = xs_buf + c + j * 64;
            const short *x2 = x1 + 32;
            vis_s16x4 a0, a1, b0, b1;
            int acc1, acc2;
            int l;

            vis_scale_min_k4(j * 2,     sc, &s1, &m1);
            vis_scale_min_k4(j * 2 + 1, sc, &s2, &m2);

            a0 = vis_zero16(); a1 = vis_zero16();
            b0 = vis_zero16(); b1 = vis_zero16();

            /* 32 weights, four at a time, two accumulators each for the
             * low and high nibble streams. Eight iterations, so each
             * accumulator sees 16 terms -- inside the 16-bit bound. */
            for (l = 0; l < 32; l += 8) {
                vis_u8x4 pl, ph;
                vis_s16x4 xv;
                unsigned int qw;

                /* Four packed bytes hold eight weights. One 32-bit
                 * load plus two masks splits them into the low-nibble
                 * and high-nibble streams without touching memory
                 * again -- SPARC's 64-bit integer ALU does this far
                 * more cheaply than VIS could. */
                qw = *(const unsigned int *) (const void *) (qs + l);
                pl = PACK_U8X4(qw & 0x0F0F0F0FU);
                ph = PACK_U8X4((qw >> 4) & 0x0F0F0F0FU);

                XLOAD4(xv, x1 + l);
                a0 = VADD16(a0, VMUL8X16(pl, xv));
                XLOAD4(xv, x2 + l);
                b0 = VADD16(b0, VMUL8X16(ph, xv));

                /* Second group of four: an independent dependency
                 * chain, so the 3-cycle VIS latency overlaps. */
                qw = *(const unsigned int *) (const void *) (qs + l + 4);
                pl = PACK_U8X4(qw & 0x0F0F0F0FU);
                ph = PACK_U8X4((qw >> 4) & 0x0F0F0F0FU);

                XLOAD4(xv, x1 + l + 4);
                a1 = VADD16(a1, VMUL8X16(pl, xv));
                XLOAD4(xv, x2 + l + 4);
                b1 = VADD16(b1, VMUL8X16(ph, xv));
            }

            acc1 = vis_hsum16(VADD16(a0, a1));
            acc2 = vis_hsum16(VADD16(b0, b1));

            sum += xq->d[blk] *
                   ((float) acc1 * d * (float) s1 -
                    (float) xq->s[blk] * dmin * (float) m1);
            sum += xq->d[blk + 1] *
                   ((float) acc2 * d * (float) s2 -
                    (float) xq->s[blk + 1] * dmin * (float) m2);

            blk += 2;
            qs  += 32;
        }
        b += 144;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Q5_K                                                                */
/*                                                                     */
/* Like Q4_K plus a qh plane supplying a fifth bit per weight, so       */
/* w is 0..31 and only 8 terms fit in a 16-bit lane                     */
/* (8 * 31 * 127 = 31496). Four accumulators, four terms each.          */
/* ------------------------------------------------------------------ */

VIS_NOINLINE static float vis_dot_q5_K(const unsigned char *b, const bk_qx *xq,
                          long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = vis_rd_f16p(b);
        const float dmin = vis_rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qh = b + 16;
        const unsigned char *qs = b + 48;
        long blk = c / 32;
        int j;

        for (j = 0; j < 4; j++) {
            unsigned char s1, m1, s2, m2;
            const short *x1 = xs_buf + c + j * 64;
            const short *x2 = x1 + 32;
            int sh1 = j * 2, sh2 = j * 2 + 1;
            vis_s16x4 a0, a1, b0, b1;
            int acc1, acc2;
            int l;

            vis_scale_min_k4(j * 2,     sc, &s1, &m1);
            vis_scale_min_k4(j * 2 + 1, sc, &s2, &m2);

            a0 = vis_zero16(); a1 = vis_zero16();
            b0 = vis_zero16(); b1 = vis_zero16();

            for (l = 0; l < 32; l += 8) {
                vis_u8x4 pl, ph;
                vis_s16x4 xv;
                unsigned int qw, hw, b4l, b4h;

                /* Same one-load split as Q4_K, plus the fifth bit from
                 * the qh plane. qh holds one bit per weight per
                 * sub-block, so the bits for four consecutive weights
                 * are scattered one byte apart; gather them with shifts
                 * and mask to 0x10 to place the bit at value 16. */
                qw = *(const unsigned int *) (const void *) (qs + l);
                hw = *(const unsigned int *) (const void *) (qh + l);

                b4l = ((hw >> sh1) & 0x01010101U) << 4;
                b4h = ((hw >> sh2) & 0x01010101U) << 4;

                pl = PACK_U8X4((qw & 0x0F0F0F0FU) | b4l);
                ph = PACK_U8X4(((qw >> 4) & 0x0F0F0F0FU) | b4h);

                XLOAD4(xv, x1 + l);
                a0 = VADD16(a0, VMUL8X16(pl, xv));
                XLOAD4(xv, x2 + l);
                b0 = VADD16(b0, VMUL8X16(ph, xv));

                qw = *(const unsigned int *) (const void *) (qs + l + 4);
                hw = *(const unsigned int *) (const void *) (qh + l + 4);

                b4l = ((hw >> sh1) & 0x01010101U) << 4;
                b4h = ((hw >> sh2) & 0x01010101U) << 4;

                pl = PACK_U8X4((qw & 0x0F0F0F0FU) | b4l);
                ph = PACK_U8X4(((qw >> 4) & 0x0F0F0F0FU) | b4h);

                XLOAD4(xv, x1 + l + 4);
                a1 = VADD16(a1, VMUL8X16(pl, xv));
                XLOAD4(xv, x2 + l + 4);
                b1 = VADD16(b1, VMUL8X16(ph, xv));

            }

            /* No mid-loop fold. w reaches 31 and |x| <= 127, so a lane
             * can take 8 terms (8 * 31 * 127 = 31496 < 32767). The loop
             * runs 4 iterations and each adds ONE term per lane, so the
             * worst case is 4 * 31 * 127 = 15748 -- half the budget.
             * The fold that used to sit here cost 12 extra hsum16 per
             * sub-block for nothing. (Finding 29.) */
            acc1 = vis_hsum16(VADD16(a0, a1));
            acc2 = vis_hsum16(VADD16(b0, b1));

            sum += xq->d[blk] *
                   ((float) acc1 * d * (float) s1 -
                    (float) xq->s[blk] * dmin * (float) m1);
            sum += xq->d[blk + 1] *
                   ((float) acc2 * d * (float) s2 -
                    (float) xq->s[blk + 1] * dmin * (float) m2);

            blk += 2;
            qs  += 32;
        }
        b += 176;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Q6_K                                                                */
/*                                                                     */
/* 256 weights per super-block in 210 bytes:                           */
/*   [ql:128][qh:64][scales:16 signed][d:fp16]                         */
/*                                                                     */
/* A weight is six bits: four from ql, two from qh, then biased by      */
/* -32. fmul8x16's first operand is UNSIGNED, so the biased value       */
/* cannot be fed to it directly. Use the identity                       */
/*                                                                     */
/*     sum((w - 32) * x) = sum(w * x) - 32 * sum(x)                     */
/*                                                                     */
/* and take sum(x) from the block sums bk_quantize_x already computes.  */
/* That is exactly what i8_dot_q6_K does, so the two agree bit for bit. */
/*                                                                     */
/* Unbiased w is 0..63 and |x| <= 127, so one product reaches 8001 and  */
/* two fit in a 16-bit lane (16002 < 32767). Pairs are added in the     */
/* 16-bit domain and widened once, which halves the widen traffic       */
/* against widening every product.                                      */
/*                                                                     */
/* The 210-byte stride means every second super-block starts 2 mod 4,   */
/* so the ql/qh planes are only ever 2-aligned. Rather than normalise   */
/* with a 192-byte memcpy per super-block -- 192 bytes of pure copy      */
/* traffic through a 16 KB direct-mapped D-cache, against 256 weights   */
/* of real work -- the loads go through vis_ldw2(), which is two lduh   */
/* and no copy at all.                                                  */
/* ------------------------------------------------------------------ */

#define Q6K_BODY(NAME, LDW)                                            \
VIS_NOINLINE static float NAME(const unsigned char *b,                 \
                               const bk_qx *xq, long c, float sum) {   \
    {                                                                  \
        const unsigned char *ql = b;                                   \
        const unsigned char *qh = b + 128;                             \
        const signed char   *sc = (const signed char *) (b + 192);     \
        const float d = vis_rd_f16p(b + 208);                          \
        int n;                                                         \
                                                                       \
        for (n = 0; n < 2; n++) {                                      \
            const unsigned char *qln = ql + n * 64;                    \
            const unsigned char *qhn = qh + n * 32;                    \
            const signed char   *scn = sc + n * 8;                     \
            long off = c + n * 128;                                    \
            long blk = off / 32;                                       \
            int a[8];                                                  \
            int g;                                                     \
                                                                       \
            /* Two groups of 16, four sub-rows each: eight accumulators\
             * matching i8_dot_q6_K's a0..a7. */                       \
            for (g = 0; g < 2; g++) {                                  \
                const short *xr0 = xs_buf + off + g * 16;              \
                const short *xr1 = xr0 + 32;                           \
                const short *xr2 = xr0 + 64;                           \
                const short *xr3 = xr0 + 96;                           \
                vis_s32x2 s0, s1, s2, s3;                              \
                vis_s16x4 t0, t1, t2, t3;                              \
                int l;                                                 \
                                                                       \
                t0 = vis_zero16(); t1 = vis_zero16();                  \
                t2 = vis_zero16(); t3 = vis_zero16();                  \
                                                                       \
                for (l = 0; l < 16; l += 8) {                          \
                    unsigned int l0w, l1w, hw, l0x, l1x, hx;           \
                    vis_u8x4 w0, w1, w2, w3;                           \
                    vis_s16x4 xv, p0, p1, p2, p3;                      \
                                                                       \
                    l0w = LDW(qln + g * 16 + l);                       \
                    l1w = LDW(qln + g * 16 + 32 + l);                  \
                    hw  = LDW(qhn + g * 16 + l);                       \
                                                                       \
                    /* Four sub-rows share one qh byte: bits 0-1 feed  \
                     * row 0, 2-3 row 1, 4-5 row 2, 6-7 row 3. */      \
                    w0 = PACK_U8X4((l0w & 0x0F0F0F0FU) |               \
                                   ((hw & 0x03030303U) << 4));         \
                    w1 = PACK_U8X4((l1w & 0x0F0F0F0FU) |               \
                                   (((hw >> 2) & 0x03030303U) << 4));  \
                    w2 = PACK_U8X4(((l0w >> 4) & 0x0F0F0F0FU) |        \
                                   (((hw >> 4) & 0x03030303U) << 4));  \
                    w3 = PACK_U8X4(((l1w >> 4) & 0x0F0F0F0FU) |        \
                                   (((hw >> 6) & 0x03030303U) << 4));  \
                                                                       \
                    XLOAD4(xv, xr0 + l); p0 = VMUL8X16(w0, xv);        \
                    XLOAD4(xv, xr1 + l); p1 = VMUL8X16(w1, xv);        \
                    XLOAD4(xv, xr2 + l); p2 = VMUL8X16(w2, xv);        \
                    XLOAD4(xv, xr3 + l); p3 = VMUL8X16(w3, xv);        \
                                                                       \
                    /* Second group of four weights: an independent    \
                     * chain, so the 3-cycle VIS latency overlaps. */  \
                    l0x = LDW(qln + g * 16 + l + 4);                   \
                    l1x = LDW(qln + g * 16 + 32 + l + 4);              \
                    hx  = LDW(qhn + g * 16 + l + 4);                   \
                                                                       \
                    w0 = PACK_U8X4((l0x & 0x0F0F0F0FU) |               \
                                   ((hx & 0x03030303U) << 4));         \
                    w1 = PACK_U8X4((l1x & 0x0F0F0F0FU) |               \
                                   (((hx >> 2) & 0x03030303U) << 4));  \
                    w2 = PACK_U8X4(((l0x >> 4) & 0x0F0F0F0FU) |        \
                                   (((hx >> 4) & 0x03030303U) << 4));  \
                    w3 = PACK_U8X4(((l1x >> 4) & 0x0F0F0F0FU) |        \
                                   (((hx >> 6) & 0x03030303U) << 4));  \
                                                                       \
                    /* Pair the two groups in 16-bit lanes before      \
                     * widening: 2 * 8001 = 16002, inside the lane. */ \
                    XLOAD4(xv, xr0 + l + 4);                           \
                    p0 = VADD16(p0, VMUL8X16(w0, xv));                 \
                    XLOAD4(xv, xr1 + l + 4);                           \
                    p1 = VADD16(p1, VMUL8X16(w1, xv));                 \
                    XLOAD4(xv, xr2 + l + 4);                           \
                    p2 = VADD16(p2, VMUL8X16(w2, xv));                 \
                    XLOAD4(xv, xr3 + l + 4);                           \
                    p3 = VADD16(p3, VMUL8X16(w3, xv));                 \
                                                                       \
                    /* Hold the products in 16-bit lanes across both   \
                     * iterations rather than widening each time. Four \
                     * terms per lane worst-case is 4 * 63 * 127 =     \
                     * 32004, inside int16. Halves the widen traffic.  \
                     * (Finding 29.) */                                \
                    t0 = VADD16(t0, p0);                               \
                    t1 = VADD16(t1, p1);                               \
                    t2 = VADD16(t2, p2);                               \
                    t3 = VADD16(t3, p3);                               \
                }                                                      \
                                                                       \
                s0 = vis_widen2(t0);                                   \
                s1 = vis_widen2(t1);                                   \
                s2 = vis_widen2(t2);                                   \
                s3 = vis_widen2(t3);                                   \
                                                                       \
                a[g]     = vis_hsum32(s0);                             \
                a[g + 2] = vis_hsum32(s1);                             \
                a[g + 4] = vis_hsum32(s2);                             \
                a[g + 6] = vis_hsum32(s3);                             \
            }                                                          \
                                                                       \
            /* Fold the -32 bias through the block sums, in the same   \
             * order i8_dot_q6_K uses so the float result matches. */  \
            sum += d * xq->d[blk] *                                    \
                   ((float) scn[0] * (float) (a[0] - 32 * vis_sum16(xq, off +  0)) +\
                    (float) scn[1] * (float) (a[1] - 32 * vis_sum16(xq, off + 16)));\
            sum += d * xq->d[blk + 1] *                                \
                   ((float) scn[2] * (float) (a[2] - 32 * vis_sum16(xq, off + 32)) +\
                    (float) scn[3] * (float) (a[3] - 32 * vis_sum16(xq, off + 48)));\
            sum += d * xq->d[blk + 2] *                                \
                   ((float) scn[4] * (float) (a[4] - 32 * vis_sum16(xq, off + 64)) +\
                    (float) scn[5] * (float) (a[5] - 32 * vis_sum16(xq, off + 80)));\
            sum += d * xq->d[blk + 3] *                                \
                   ((float) scn[6] * (float) (a[6] - 32 * vis_sum16(xq, off + 96)) +\
                    (float) scn[7] * (float) (a[7] - 32 * vis_sum16(xq, off + 112)));\
        }                                                              \
    }                                                                  \
    return sum;                                                        \
}

Q6K_BODY(vis_dot_q6_K_al, VIS_LDW)
Q6K_BODY(vis_dot_q6_K_un, vis_ldw2)

/* Pick once per row, not per load. Q6_K's 210-byte stride alternates
 * the alignment of each super-block, but a row's first super-block
 * settles it for the whole row when ncols is a multiple of 512 (two
 * super-blocks, 420 bytes, even); otherwise fall back to the safe
 * halfword path. The aligned variant is the one that runs on every
 * tensor in the model. */
static float vis_dot_q6_K(const unsigned char *b, const bk_qx *xq,
                          long ncols) {
    float sum = 0.0f;
    long c;

    /* The 210-byte stride makes alignment alternate 0,2,0,2,... within
     * a row, so the choice has to be made per super-block, not per row.
     * Half the blocks take the aligned lduw path for free; the other
     * half take two lduh. No copy either way. */
    for (c = 0; c < ncols; c += QK_K) {
        if ((((unsigned long) b) & 3UL) == 0UL) {
            sum = vis_dot_q6_K_al(b, xq, c, sum);
        } else {
            sum = vis_dot_q6_K_un(b, xq, c, sum);
        }
        b += 210;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/*                                                                     */
/* Formats without a VIS kernel fall through to the portable integer    */
/* path, which is what bk_qmv_i8_fallback() provides. Correctness is    */
/* never at risk from a missing kernel.                                 */
/* ------------------------------------------------------------------ */

void bk_qmv_vis(int type, const unsigned char *w, const float *x,
                float *y, long ncols, long nrows) {
    const bk_qx *xq;
    long rowbytes, r;

    if (type != GGML_TYPE_Q4_K && type != GGML_TYPE_Q5_K &&
        type != GGML_TYPE_Q6_K) {
        qmv_i8(type, w, x, y, ncols, nrows);
        return;
    }

    xq = bk_quantize_x(x, ncols);
    xs_prepare(xq);
    rowbytes = gg_type_size(type, ncols);

    if (type == GGML_TYPE_Q4_K) {
        for (r = 0; r < nrows; r++) {
            y[r] = vis_dot_q4_K(w + r * rowbytes, xq, ncols);
        }
    } else if (type == GGML_TYPE_Q5_K) {
        for (r = 0; r < nrows; r++) {
            y[r] = vis_dot_q5_K(w + r * rowbytes, xq, ncols);
        }
    } else {
        for (r = 0; r < nrows; r++) {
            y[r] = vis_dot_q6_K(w + r * rowbytes, xq, ncols);
        }
    }
}

/* Runtime detection. VIS is present on every UltraSPARC, so the
 * question is whether this binary was built for one. There is no
 * CPUID equivalent; Solaris exposes it through getisax(), which is
 * not available everywhere, so the compile-time target is used. */
int bk_vis_available(void) {
    return 1;
}

#endif /* INFER_HAVE_VIS */
