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
 * SIGNED WEIGHTS: the bias trick
 * ---------------------------------------------------------------
 *
 * fmul8x16's first operand is *unsigned*. VIS 1 has no signed 8-bit
 * multiply (fmul8sux16 is s8 x u16, which breaks for negative
 * activations), so a format whose weights can go negative cannot be
 * fed in directly.
 *
 * The fix is to add a constant to the weights and subtract its
 * contribution from the result:
 *
 *     sum (w * x) = sum ((w + B) * x) - B * sum(x)
 *
 * The middle term is a plain u8 multiply and therefore exact. The
 * correction uses the per-block sums of the quantised activations,
 * which the shared backend already computes (xq->s for 32-element
 * blocks, xq->s16 for 16-element blocks), so it costs one integer
 * multiply per block -- and it is exactly the arithmetic the portable
 * i8 kernel performs, so results stay bit-identical to it.
 *
 * Bias constants and their lane budgets:
 *
 *     Q6_K  w in -32..31  ->  w+32 in 0..63   4 lanes * 63*127  ok
 *     Q8_0  q in -128..127 -> q+128 in 0..255 1 lane  * 255*127 ok
 *     Q4_0  w in -8..7    ->  w+8  in 0..23   8 lanes * 23*127  ok
 *
 * Q6_K and Q8_0 accumulate straight into 32-bit lanes (fmuld8ulx16
 * widening is exact), because their biased products are too large to
 * stack in 16-bit lanes.
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
 *
 * Q6_K / Q8_0 / Q4_0 widen to 32-bit lanes as they go (see above).
 * Widening is done with fmuld8ulx16 against a constant 1 byte, which
 * is an exact 16 -> 32 bit expansion of two lanes and avoids needing
 * an unpack instruction VIS does not have.
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

/* Lane access goes through these unions, never through subscripts:
 * GCC lowers v[0]/v[1] to stack round-trips on SPARC (~85 instructions
 * of store/load noise for one fold). Memory-order halves are widened
 * and summed, so the byte-order details cannot matter. */
typedef union {
    vis_s16x4     v;
    unsigned long u;
} vis_pun64;

typedef union {
    vis_u8x4     v;
    unsigned int u;
} vis_pun32;

typedef union {
    vis_s16x2     v;
    unsigned int u;
} vis_pun32s;

typedef union {
    vis_s32x2     v;
    unsigned long u;
} vis_pun64i;

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

/* Widen all four 16-bit lanes of v to two 32-bit lanes (exact). */
static vis_s32x2 vis_widen2(vis_s16x4 v) {
    vis_lane64 in;
    vis_lane32 one;
    vis_wide64 w;

    one.u = 0x01010101U;
    in.d  = v;
    w.d   = VADD32(VWIDEN(one.f, in.f[0]), VWIDEN(one.f, in.f[1]));
    return w.d;
}

/* Horizontal sum of two 32-bit lanes to int. */
static int vis_hsum32(vis_s32x2 v) {
    vis_wide64 w;
    w.d = v;
    return w.i[0] + w.i[1];
}

static vis_s32x2 vis_zero32(void) {
    return vis_fzero();
}

#else

static vis_u8x4 vis_one8(void) {
    vis_pun32 p;
    p.u = 0x01010101U;
    return p.v;
}

static int vis_hsum16(vis_s16x4 v) {
    vis_pun64 pu;
    vis_pun32s lo, hi;
    vis_s32x2 wl, wh, s;

    pu.v = v;
    lo.u = (unsigned int) pu.u;
    hi.u = (unsigned int) (pu.u >> 32);

    wl = VWIDEN(vis_one8(), lo.v);
    wh = VWIDEN(vis_one8(), hi.v);
    s  = VADD32(wl, wh);
    return s[0] + s[1];
}

static vis_s16x4 vis_zero16(void) {
    vis_pun64 p;
    p.u = 0UL;
    return p.v;
}

static vis_s32x2 vis_widen2(vis_s16x4 v) {
    vis_pun64 pu;
    vis_pun32s lo, hi;
    vis_s32x2 w;

    pu.v = v;
    lo.u = (unsigned int) pu.u;
    hi.u = (unsigned int) (pu.u >> 32);
    w = VADD32(VWIDEN(vis_one8(), lo.v), VWIDEN(vis_one8(), hi.v));
    return w;
}

static int vis_hsum32(vis_s32x2 v) {
    vis_pun64i pu;
    pu.v = v;
    return (int) (unsigned int) pu.u +
           (int) (unsigned int) (pu.u >> 32);
}

static vis_s32x2 vis_zero32(void) {
    vis_pun64i p;
    p.u = 0UL;
    return p.v;
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

/* Four weight bytes into a u8x4 in memory order.
 *
 * Q6_K rows are 210*nb bytes and Q4_0/Q8_0 rows 18*nb / 34*nb, none of
 * which is a multiple of four in general, so a block can start at
 * offset 2 mod 4 and a plain lduw would fault on SPARC. A 4-byte
 * memcpy is the compiler's job to lower: a single lduw where it can
 * prove alignment, a safe sequence where it cannot, and the same byte
 * order on both hosts either way. */
/* Tell GCC a pointer is aligned so loads compile to single lduw/lduh
 * instead of byte-wise sequences; Sun Studio has no equivalent and
 * emits safe code. Only used where alignment is guaranteed by
 * construction: Q6_K normalises super-blocks to 4 bytes, and the
 * 32-block formats' weight bytes sit at +2 inside even-strided rows,
 * so they are 2-byte aligned. */
#if defined(__GNUC__)
#define VIS_ALIGN4_(p, a) \
    ((const unsigned char *) __builtin_assume_aligned((const void *) (p), (a)))
#define VIS_ALIGN4(p) VIS_ALIGN4_(p, 4)
#else
#define VIS_ALIGN4_(p, a) ((const unsigned char *) (p))
#define VIS_ALIGN4(p) ((const unsigned char *) (p))
#endif

static unsigned int vis_ld_u32e(const unsigned char *p) {
    unsigned int u;
    memcpy(&u, p, 4);
    return u;
}

/* Four bytes at any 2-byte-aligned address, assembled into a word in
 * native byte order. Q8_0 and Q4_0 store their weight bytes at offset
 * +2 inside 34-/18-byte blocks, so they are never 4-aligned; a memcpy
 * here would make GCC emit four ldub plus four stb. Two lduh are
 * legal (the stride is even) and compile to shift+or. */
static unsigned int vis_ld_u32a(const unsigned char *p) {
    const unsigned char *q = VIS_ALIGN4_(p, 2);
    return (unsigned int) (q[0] | (q[1] << 8)) |
           ((unsigned int) (q[2] | (q[3] << 8)) << 16);
}

/* The five dot kernels are called once per matrix row from the
 * dispatcher; keep them as real functions (per-row call overhead is a
 * few instructions against hundreds per row) and let GCC count them as
 * such. Sun Studio has no equivalent and may inline at -xO5, which is
 * equally fine. */
#if defined(__GNUC__)
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
             * accumulator sees 16 terms -- inside the 16-bit bound.
             * Two 16-term chains cannot be merged in 16-bit lanes
             * (32 * 15 * 127 overflows), but their widened 32-bit
             * lanes can: one widen+add pair per chain, then a single
             * horizontal sum per stream. */
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

            acc1 = vis_hsum32(VADD32(vis_widen2(a0), vis_widen2(a1)));
            acc2 = vis_hsum32(VADD32(vis_widen2(b0), vis_widen2(b1)));

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

/* Fold four 8-term s16 chains of one stream into a single int: widen
 * each chain to a 32-bit lane pair (fmuld8ulx16, exact) and add.
 * Defined before vis_dot_q5_K so both compilers can inline it. */
#if defined(VIS_SUNPRO)
static int vis_fold4(vis_s16x4 c0, vis_s16x4 c1,
                     vis_s16x4 c2, vis_s16x4 c3) {
    vis_lane64 in;
    vis_lane32 one;
    vis_wide64 s;

    one.u = 0x01010101U;
    in.d = c0;
    s.d  = VADD32(VWIDEN(one.f, in.f[0]), VWIDEN(one.f, in.f[1]));
    in.d = c1;
    s.d  = VADD32(s.d, VADD32(VWIDEN(one.f, in.f[0]), VWIDEN(one.f, in.f[1])));
    in.d = c2;
    s.d  = VADD32(s.d, VADD32(VWIDEN(one.f, in.f[0]), VWIDEN(one.f, in.f[1])));
    in.d = c3;
    s.d  = VADD32(s.d, VADD32(VWIDEN(one.f, in.f[0]), VWIDEN(one.f, in.f[1])));
    return s.i[0] + s.i[1];
}
#else
static int vis_fold4(vis_s16x4 c0, vis_s16x4 c1,
                     vis_s16x4 c2, vis_s16x4 c3) {
    vis_pun64 pu;
    vis_pun32s lo, hi;
    vis_s32x2 s;
    unsigned long u0, u1, u2, u3;

    pu.v = c0; u0 = pu.u;
    pu.v = c1; u1 = pu.u;
    pu.v = c2; u2 = pu.u;
    pu.v = c3; u3 = pu.u;

    lo.u = (unsigned int) u0; hi.u = (unsigned int) (u0 >> 32);
    s = VADD32(VWIDEN(vis_one8(), lo.v), VWIDEN(vis_one8(), hi.v));
    lo.u = (unsigned int) u1; hi.u = (unsigned int) (u1 >> 32);
    s = VADD32(s, VADD32(VWIDEN(vis_one8(), lo.v), VWIDEN(vis_one8(), hi.v)));
    lo.u = (unsigned int) u2; hi.u = (unsigned int) (u2 >> 32);
    s = VADD32(s, VADD32(VWIDEN(vis_one8(), lo.v), VWIDEN(vis_one8(), hi.v)));
    lo.u = (unsigned int) u3; hi.u = (unsigned int) (u3 >> 32);
    s = VADD32(s, VADD32(VWIDEN(vis_one8(), lo.v), VWIDEN(vis_one8(), hi.v)));
    return s[0] + s[1];
}
#endif

/* ------------------------------------------------------------------ */
/* Q5_K                                                                */
/*                                                                     */
/* Like Q4_K plus a qh plane supplying a fifth bit per weight, so       */
/* w is 0..31 and only 8 terms fit in a 16-bit lane                     */
/* (8 * 31 * 127 = 31496).                                              */
/*                                                                     */
/* Eight chains of eight terms (a0..a3 for the low-nibble stream,       */
/* b0..b3 for the high) run the whole sub-block without folding: each   */
/* chain collects exactly eight terms over the four loop iterations     */
/* and is widened once at the end. An earlier version folded to 32-bit  */
/* every iteration (four hsums plus four fzeros per eight weights),     */
/* which cost more than the multiply it guarded.                        */
/* ------------------------------------------------------------------ */

#define Q5K_GROUP(DSTL, DSTH, OFF)                                       \
    do {                                                                 \
        unsigned int qw_, hw_, b4l_, b4h_;                               \
        vis_u8x4 pl_, ph_;                                               \
        vis_s16x4 xv_;                                                   \
        qw_ = *(const unsigned int *) (const void *) (qs + (OFF));       \
        hw_ = *(const unsigned int *) (const void *) (qh + (OFF));       \
        b4l_ = ((hw_ >> sh1) & 0x01010101U) << 4;                        \
        b4h_ = ((hw_ >> sh2) & 0x01010101U) << 4;                        \
        pl_ = PACK_U8X4((qw_ & 0x0F0F0F0FU) | b4l_);                     \
        ph_ = PACK_U8X4(((qw_ >> 4) & 0x0F0F0F0FU) | b4h_);              \
        XLOAD4(xv_, x1 + (OFF));                                         \
        DSTL = VADD16(DSTL, VMUL8X16(pl_, xv_));                         \
        XLOAD4(xv_, x2 + (OFF));                                         \
        DSTH = VADD16(DSTH, VMUL8X16(ph_, xv_));                         \
    } while (0)

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
            vis_s16x4 a0, a1, a2, a3, b0, b1, b2, b3;
            int acc1, acc2, acc32_1, acc32_2;
            int l;

            vis_scale_min_k4(j * 2,     sc, &s1, &m1);
            vis_scale_min_k4(j * 2 + 1, sc, &s2, &m2);

            a0 = vis_zero16(); a1 = vis_zero16();
            a2 = vis_zero16(); a3 = vis_zero16();
            b0 = vis_zero16(); b1 = vis_zero16();
            b2 = vis_zero16(); b3 = vis_zero16();

            for (l = 0; l < 32; l += 16) {
                /* First half: chains 0 and 1. */
                Q5K_GROUP(a0, b0, l);
                Q5K_GROUP(a1, b1, l + 4);
                /* Second half: chains 2 and 3, so each chain sees two
                 * 4-weight groups = 8 terms. */
                Q5K_GROUP(a2, b2, l + 8);
                Q5K_GROUP(a3, b3, l + 12);
            }

            /* Fold each stream's four 8-term chains straight to one
             * 32-bit pair; no 16-bit merge (8+8 would overflow). */
            acc32_1 = vis_fold4(a0, a1, a2, a3);
            acc32_2 = vis_fold4(b0, b1, b2, b3);
            acc1 = acc32_1;
            acc2 = acc32_2;

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
/*   [ql:128 nibble bytes][qh:64 hi-bit bytes][sc:16 int8][d:fp16]     */
/*                                                                     */
/* Four 32-weight rows share the ql bytes in pairs (rows 0/2 read      */
/* ql[l], rows 1/3 read ql[l+32]) and each qh byte carries two hi bits */
/* per row, at bit positions 2r and 2r+1. The true weight is           */
/* (nibble | hi2 << 4) - 32, i.e. signed. Biased by +32 it becomes     */
/* the u8 byte the multiply needs; the -32 * sum(x) correction is      */
/* applied per 16-weight scale group from xq->s16, exactly like the    */
/* portable i8 kernel. Biased products are at most 63*127 = 8001, so   */
/* two of them fit a 16-bit lane; they are paired, then widened        */
/* straight into a 32-bit lane pair per row.                           */
/* ------------------------------------------------------------------ */

VIS_NOINLINE static float vis_dot_q6_K(const unsigned char *b, const bk_qx *xq,
                          long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        long qbuf_l[192 / (long) sizeof(long)];   /* 8-aligned by C */
        const unsigned char *ql = b;
        const unsigned char *qh;
        const signed char   *sc = (const signed char *) (b + 192);
        const float d = vis_rd_f16p(b + 208);
        int n;

        /* Super-blocks advance by 210 bytes, so their base alternates
         * between aligned and 2-mod-4; a plain lduw on the ql/qh plane
         * would fault on the odd ones. Normalise a misaligned block
         * into a stack copy once, keeping the inner loop aligned. */
        if ((((unsigned long) b) & 3UL) != 0UL) {
            memcpy(qbuf_l, b, 192);
            ql = (const unsigned char *) qbuf_l;
        }
        ql = VIS_ALIGN4(ql);
        qh = VIS_ALIGN4(ql + 128);

        for (n = 0; n < 2; n++) {
            const unsigned char *qlp = VIS_ALIGN4(ql + n * 64);
            const unsigned char *qhp = VIS_ALIGN4(qh + n * 32);
            const signed char *scn = sc + n * 8;
            long off = c + n * 128;
            int accs[4][2];
            int g, r;

            for (g = 0; g < 2; g++) {
                const short *xr0 = xs_buf + off + g * 16;
                const short *xr1 = xr0 + 32;
                const short *xr2 = xr0 + 64;
                const short *xr3 = xr0 + 96;
                vis_s32x2 a0, b0, c0, d0;
                int l;
                a0 = vis_zero32(); b0 = vis_zero32();
                c0 = vis_zero32(); d0 = vis_zero32();

                for (l = 0; l < 16; l += 8) {
                    /* Two 4-weight groups; the products of each row are
                     * paired in 16-bit lanes (2 * 8001 fits) before the
                     * widen, halving the widen/add traffic. */
                    unsigned int l0w, l1w, hw;
                    vis_u8x4 w00, w01, w02, w03, w10, w11, w12, w13;
                    vis_s16x4 p00, p01, p02, p03, p10, p11, p12, p13;
                    vis_s16x4 xv;

                    l0w = vis_ld_u32e(qlp + g * 16 + l);
                    l1w = vis_ld_u32e(qlp + g * 16 + 32 + l);
                    hw  = vis_ld_u32e(qhp + g * 16 + l);

                    /* row 0: low nibble of ql, hi bits 0-1 of qh */
                    w00 = PACK_U8X4((l0w & 0x0F0F0F0FU) |
                                    ((hw & 0x03030303U) << 4));
                    /* row 1: low nibble of ql+32, hi bits 2-3 */
                    w01 = PACK_U8X4((l1w & 0x0F0F0F0FU) |
                                    (((hw >> 2) & 0x03030303U) << 4));
                    /* row 2: high nibble of ql, hi bits 4-5 */
                    w02 = PACK_U8X4(((l0w >> 4) & 0x0F0F0F0FU) |
                                    (((hw >> 4) & 0x03030303U) << 4));
                    /* row 3: high nibble of ql+32, hi bits 6-7 */
                    w03 = PACK_U8X4(((l1w >> 4) & 0x0F0F0F0FU) |
                                    (((hw >> 6) & 0x03030303U) << 4));

                    XLOAD4(xv, xr0 + l);
                    p00 = VMUL8X16(w00, xv);
                    XLOAD4(xv, xr1 + l);
                    p01 = VMUL8X16(w01, xv);
                    XLOAD4(xv, xr2 + l);
                    p02 = VMUL8X16(w02, xv);
                    XLOAD4(xv, xr3 + l);
                    p03 = VMUL8X16(w03, xv);

                    l0w = vis_ld_u32e(qlp + g * 16 + l + 4);
                    l1w = vis_ld_u32e(qlp + g * 16 + 32 + l + 4);
                    hw  = vis_ld_u32e(qhp + g * 16 + l + 4);

                    w10 = PACK_U8X4((l0w & 0x0F0F0F0FU) |
                                    ((hw & 0x03030303U) << 4));
                    w11 = PACK_U8X4((l1w & 0x0F0F0F0FU) |
                                    (((hw >> 2) & 0x03030303U) << 4));
                    w12 = PACK_U8X4(((l0w >> 4) & 0x0F0F0F0FU) |
                                    (((hw >> 4) & 0x03030303U) << 4));
                    w13 = PACK_U8X4(((l1w >> 4) & 0x0F0F0F0FU) |
                                    (((hw >> 6) & 0x03030303U) << 4));

                    XLOAD4(xv, xr0 + l + 4);
                    p10 = VMUL8X16(w10, xv);
                    XLOAD4(xv, xr1 + l + 4);
                    p11 = VMUL8X16(w11, xv);
                    XLOAD4(xv, xr2 + l + 4);
                    p12 = VMUL8X16(w12, xv);
                    XLOAD4(xv, xr3 + l + 4);
                    p13 = VMUL8X16(w13, xv);

                    a0 = VADD32(a0, vis_widen2(VADD16(p00, p10)));
                    b0 = VADD32(b0, vis_widen2(VADD16(p01, p11)));
                    c0 = VADD32(c0, vis_widen2(VADD16(p02, p12)));
                    d0 = VADD32(d0, vis_widen2(VADD16(p03, p13)));
                }

                /* Store the per-row 32-bit accumulators, then fold both
                 * scale groups per row in the same order as the i8
                 * kernel, so the float arithmetic is bit-identical. */
                accs[0][g] = vis_hsum32(a0);
                accs[1][g] = vis_hsum32(b0);
                accs[2][g] = vis_hsum32(c0);
                accs[3][g] = vis_hsum32(d0);
            }

            for (r = 0; r < 4; r++) {
                long blk = (off + r * 32) / 32;
                long si  = (off + r * 32) / 16;
                sum += d * xq->d[blk] *
                       ((float) scn[2 * r] * (float) (accs[r][0] - 32 * xq->s16[si]) +
                        (float) scn[2 * r + 1] * (float) (accs[r][1] - 32 * xq->s16[si + 1]));
            }
        }
        b += 210;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Q8_0                                                                */
/*                                                                     */
/* 32 signed bytes per 34-byte block. Biased by +128 they are the u8   */
/* bytes the multiply wants, and the -128 * sum(x) correction uses the */
/* 32-element block sum. A biased product can be 255*127 = 32385, so   */
/* one term at a time is widened straight into 32-bit lanes.           */
/* ------------------------------------------------------------------ */

VIS_NOINLINE static float vis_dot_q8_0(const unsigned char *b, const bk_qx *xq,
                          long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += 32) {
        const float d = vis_rd_f16p(b);
        const unsigned char *q = b + 2;
        const short *xp = xs_buf + c;
        vis_s32x2 acc;
        int l;

        acc = vis_zero32();

        for (l = 0; l < 32; l += 4) {
            vis_u8x4 qv;
            vis_s16x4 xv, p;

            /* Flip the sign bit: the raw byte of a signed q is q+256 as
             * u8, but the bias needs q+128. XOR is exact and costs one
             * integer op; the pack is the price of the integer domain. */
            qv = PACK_U8X4(vis_ld_u32a(q + l) ^ 0x80808080U);
            XLOAD4(xv, xp + l);
            p = VMUL8X16(qv, xv);
            acc = VADD32(acc, vis_widen2(p));
        }

        sum += d * xq->d[c / 32] *
               (float) (vis_hsum32(acc) - 128 * xq->s[c / 32]);
        b += 34;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Q4_0                                                                */
/*                                                                     */
/* 16 nibble bytes per 18-byte block; w = nibble - 8. The raw nibble    */
/* is already a legal u8 operand (the -8 is folded into the per-block   */
/* correction 8 * sum(x), exactly as the i8 kernel folds it), and eight */
/* terms fit a 16-bit lane (8 * 15 * 127 = 15240).                      */
/* ------------------------------------------------------------------ */

VIS_NOINLINE static float vis_dot_q4_0(const unsigned char *b, const bk_qx *xq,
                          long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += 32) {
        const float d = vis_rd_f16p(b);
        const unsigned char *q = b + 2;
        const short *x1 = xs_buf + c;
        const short *x2 = x1 + 16;
        vis_s16x4 a0, a1, b0, b1;
        int l;

        a0 = vis_zero16(); a1 = vis_zero16();
        b0 = vis_zero16(); b1 = vis_zero16();

        for (l = 0; l < 16; l += 8) {
            unsigned int qw;
            vis_u8x4 pl, ph;
            vis_s16x4 xv;

            qw = vis_ld_u32a(q + l);
            pl = PACK_U8X4(qw & 0x0F0F0F0FU);
            ph = PACK_U8X4((qw >> 4) & 0x0F0F0F0FU);

            XLOAD4(xv, x1 + l);
            a0 = VADD16(a0, VMUL8X16(pl, xv));
            XLOAD4(xv, x2 + l);
            b0 = VADD16(b0, VMUL8X16(ph, xv));

            qw = vis_ld_u32a(q + l + 4);
            pl = PACK_U8X4(qw & 0x0F0F0F0FU);
            ph = PACK_U8X4((qw >> 4) & 0x0F0F0F0FU);

            XLOAD4(xv, x1 + l + 4);
            a1 = VADD16(a1, VMUL8X16(pl, xv));
            XLOAD4(xv, x2 + l + 4);
            b1 = VADD16(b1, VMUL8X16(ph, xv));
        }

        /* One combined term, exactly as i8_dot_q4_0 folds it, so the
         * float arithmetic is bit-identical. */
        sum += d * xq->d[c / 32] *
               (float) (vis_hsum16(a0) + vis_hsum16(a1) +
                        vis_hsum16(b0) + vis_hsum16(b1) -
                        8 * xq->s[c / 32]);
        b += 18;
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
        type != GGML_TYPE_Q6_K && type != GGML_TYPE_Q8_0 &&
        type != GGML_TYPE_Q4_0) {
        qmv_i8(type, w, x, y, ncols, nrows);
        return;
    }

    /* The k-quant kernels iterate whole 256-weight super-blocks; the
     * 32-block kernels need a full block. Anything else is not ours. */
    if ((type == GGML_TYPE_Q6_K || type == GGML_TYPE_Q4_K ||
         type == GGML_TYPE_Q5_K) && (ncols % QK_K) != 0) {
        qmv_i8(type, w, x, y, ncols, nrows);
        return;
    }
    if ((type == GGML_TYPE_Q8_0 || type == GGML_TYPE_Q4_0) &&
        (ncols % 32) != 0) {
        qmv_i8(type, w, x, y, ncols, nrows);
        return;
    }

    xq = bk_quantize_x(x, ncols);
    xs_prepare(xq);
    rowbytes = gg_type_size(type, ncols);

    switch (type) {
        case GGML_TYPE_Q4_K:
            for (r = 0; r < nrows; r++) {
                y[r] = vis_dot_q4_K(w + r * rowbytes, xq, ncols);
            }
            return;
        case GGML_TYPE_Q5_K:
            for (r = 0; r < nrows; r++) {
                y[r] = vis_dot_q5_K(w + r * rowbytes, xq, ncols);
            }
            return;
        case GGML_TYPE_Q6_K:
            for (r = 0; r < nrows; r++) {
                y[r] = vis_dot_q6_K(w + r * rowbytes, xq, ncols);
            }
            return;
        case GGML_TYPE_Q8_0:
            for (r = 0; r < nrows; r++) {
                y[r] = vis_dot_q8_0(w + r * rowbytes, xq, ncols);
            }
            return;
        case GGML_TYPE_Q4_0:
            for (r = 0; r < nrows; r++) {
                y[r] = vis_dot_q4_0(w + r * rowbytes, xq, ncols);
            }
            return;
        default:
            return;
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
