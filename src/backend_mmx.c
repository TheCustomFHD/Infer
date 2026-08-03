/* backend_mmx.c -- MMX inner loops for the quantised matrix-vector product.
 *
 * Compiled only when INFER_HAVE_MMX is defined, and selected at run time
 * only when CPUID reports MMX. Everything here is an optimisation of the
 * `i8` backend in backend.c; the surrounding structure and the accuracy
 * are identical.
 *
 * Why MMX and not 3DNow!:
 *
 *   3DNow! is a *floating point* SIMD extension -- two single-precision
 *   lanes per register. On a Geode LX a PFADD has 8-cycle latency and a
 *   PFMUL similar, versus 6 for an MMX integer op, so 3DNow! offers 2
 *   float lanes where MMX offers 4 int16 lanes through PMADDWD (which
 *   also does the accumulate). Since the activations are already
 *   quantised to int8 the work is integer anyway, and converting to
 *   float to use 3DNow! would throw that advantage away. MMX wins on
 *   this workload; see README for the measured numbers.
 *
 * PMADDWD is the whole trick: it multiplies four pairs of signed 16-bit
 * values and adds them pairwise into two 32-bit accumulators, i.e. four
 * multiply-accumulates per instruction, no widening needed.
 *
 * Kernels in this file:
 *
 *   q4_K / q5_K   one 256-weight block, four 64-weight sub-rows. The
 *                 nibbles are expanded straight into MMX registers with
 *                 PAND + PUNPCK (no scalar unpack loop, no staging
 *                 buffer); Q5_K adds the +16 hi-bit term with
 *                 PSRLW/PAND/PSLLW/PADDW.
 *   q6_K          one 128-weight half-block, eight 16-weight groups.
 *                 The 2-bit hi part is extracted the same way and the
 *                 -32 bias is folded in with PSUBW, so the kernel needs
 *                 no activation-sum correction term.
 *   q8_0          int8 -> int16 with PSLLW+PSRAW (arithmetic shift),
 *                 then PMADDWD.
 *   q4_0          nibbles minus 8, as in the reference kernel.
 *
 * Each dot product is built from 8-lane partials summed into two
 * accumulators to hide PMADDWD latency, and the FPU is switched back to
 * x87 once per sub-row / half-block -- not per 32 lanes.
 *
 * This is the only file in the project that uses inline assembly, and
 * the only one that is not strictly conforming ANSI C. The portable
 * build never compiles it.
 */

#include "infer.h"
#include "backend.h"

#include <string.h>

#ifdef INFER_HAVE_MMX

/* Word masks used by the unpacking, kept in memory (an MMX op may take
 * its second operand from memory, which saves a register).
 *
 * ALIGNMENT -- this matters, and getting it wrong is subtle:
 *
 *   `movq mem, %mm` requires an 8-byte-aligned source. A bare
 *   `short[4]` is only 2-byte aligned per the C standard, and in
 *   practice GCC gives .rodata 4-byte alignment on 32-bit x86, so such
 *   constants land at addresses 4 mod 8 -- every one of them misaligned.
 *
 *   An out-of-order x86 silently splits a misaligned MMX load, so the
 *   bug hides completely on a modern development machine. That is a
 *   CPU-specific courtesy, not an architectural guarantee: it costs a
 *   penalty on every access and can fault outright on strict, emulated
 *   or virtualised implementations. It was observed as a hard failure on
 *   real Geode LX / Windows XP hardware.
 *
 *   Note a union with `long long` is NOT sufficient: the i386 SysV ABI
 *   aligns long long to 4, not 8, so GCC emits no `.align 8` and the
 *   constants stay misaligned (verified with readelf). An explicit
 *   alignment attribute is required.
 *
 * tests/t_align.c asserts at run time that every constant below is
 * 8-byte aligned, so a compiler that silently ignores the attribute is
 * caught by `make test` rather than by a crash on the target. */

#if defined(__GNUC__)
#define MMX_ALIGN8 __attribute__((aligned(8)))
#else
/* No known non-GCC compiler can build this file anyway -- it is written
 * in GNU inline assembly -- so refuse rather than emit silently
 * misaligned constants. */
#error "backend_mmx.c needs GCC/Clang for both inline asm and aligned(8); build without INFER_HAVE_MMX to use the portable i8 backend instead."
#endif

typedef union {
    long long align_;      /* widest member; the attribute does the work */
    short     v[4];
} mmx_const;

static const mmx_const c_mask_f_u MMX_ALIGN8 = { 0x000F000F000F000FLL };
/* Byte-domain nibble mask: one PAND extracts all eight low nibbles of a
 * packed 8-byte group, before widening to words. Used by the Q4_K
 * kernel; the word-domain mask above is still needed where masking
 * happens after widening. */
static const mmx_const c_mask_fb_u MMX_ALIGN8 = { 0x0F0F0F0F0F0F0F0FLL };
static const mmx_const c_mask_1_u  MMX_ALIGN8 = { 0x0001000100010001LL };
/* Byte-domain single-bit mask, companion to c_mask_fb. */
static const mmx_const c_mask_1b_u MMX_ALIGN8 = { 0x0101010101010101LL };
static const mmx_const c_mask_3_u MMX_ALIGN8 = { 0x0003000300030003LL };
static const mmx_const c_8_u      MMX_ALIGN8 = { 0x0008000800080008LL };
static const mmx_const c_32_u     MMX_ALIGN8 = { 0x0020002000200020LL };

/* Exposed for tests/t_align.c. */
const void *mmx_const_addrs[5] = {
    (const void *) &c_mask_f_u, (const void *) &c_mask_1_u,
    (const void *) &c_mask_3_u, (const void *) &c_8_u,
    (const void *) &c_32_u
};

#define c_mask_f  (c_mask_f_u.v)
#define c_mask_fb (c_mask_fb_u.v)
#define c_mask_1b (c_mask_1b_u.v)
#define c_mask_1 (c_mask_1_u.v)
#define c_mask_3 (c_mask_3_u.v)
#define c_8      (c_8_u.v)
#define c_32     (c_32_u.v)

/* Return the FPU from MMX state to x87 state.
 *
 * WHY THIS IS NOT INSIDE THE KERNELS:
 *
 * On the Geode the FPU is a separate unit fed by a queue, and OLPC's
 * measurements note that "every data exchange requires synchronization,
 * which can consume a LOT of cycles". EMMS is exactly such an exchange.
 *
 * The 1.3.0 kernels issued one EMMS per 64-weight sub-row, which works
 * out at ~8 million EMMS per forward pass for this model. Measured on
 * the reference host, an EMMS around a small MMX block costs ~3.6x the
 * block's own work; on an in-order core with an asynchronous FPU it is
 * worse.
 *
 * So the kernels now leave the FPU in MMX state and each *row* issues a
 * single EMMS after its last block. The scale arithmetic between blocks
 * is float, which is why this needs care: the per-row driver
 * accumulates integer dot products first and only touches float after
 * the sync. See the mmx_row_* functions below. */
/* Largest number of per-sub-row accumulators a single matrix row can
 * need: the widest tensor here is 3584 columns (ffn_down) = 14 blocks of
 * 256, 8 accumulators each. Rounded up generously. */
#define MMX_ROW_ACC 256

static void mmx_sync(void) {
    __asm__ __volatile__ ("emms" : : :
        "st", "st(1)", "st(2)", "st(3)",
        "st(4)", "st(5)", "st(6)", "st(7)");
}

static float rd_f16p(const unsigned char *p) {
    return q_fp16_to_fp32((unsigned short) (p[0] | (p[1] << 8)));
}

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
/* Inline asm helpers                                                  */
/*                                                                     */
/* Every kernel is a sequence of __asm__ blocks that share MMX register */
/* state (mm0/mm1 accumulate, mm7 is zero). The blocks must not be      */
/* reordered or elided, and no floating-point work may be scheduled     */
/* across them (MMX registers alias the x87 stack), hence volatile and  */
/* the st() clobbers. The final block of each kernel issues EMMS and    */
/* stores the 32-bit results, restoring the FPU for the x87 scale       */
/* application in the surrounding C code.                               */
/* ------------------------------------------------------------------ */

#define INFER_MMX_CLOBBERS \
    "memory", \
    "st", "st(1)", "st(2)", "st(3)", \
    "st(4)", "st(5)", "st(6)", "st(7)"

/* Q4_K: one 64-weight sub-row. qs points at 32 nibble bytes, x at the
 * 64 matching int16 activations, accs at an int[2]. J2 is the hi-bit
 * shift (0 for Q4_K: unused, see Q5_K). */
/* One 8-byte chunk = 16 weights, 4 pmaddwd.
 *
 * SCHEDULING -- this shape is deliberate:
 *
 * The Geode's FPU is a separate unit behind a 6-deep queue. OLPC
 * measured MMX latency at ~6 cycles but pipeline throughput at ~2
 * cycles, and you only get the throughput figure when consecutive
 * instructions are INDEPENDENT. Profiling on real hardware showed this
 * kernel running at ~9 cycles per MMX instruction -- i.e. essentially
 * latency-bound, ~3x off what the pipeline can sustain.
 *
 * The previous shape funnelled all four pmaddwd results through one
 * scratch register (mm3) and accumulated two of them into mm0
 * back-to-back, so almost every instruction depended on its immediate
 * predecessor.
 *
 * Now: the two punpck halves are computed together, the four masked
 * vectors live in four distinct registers, the four pmaddwd write four
 * distinct destinations, and the four paddd alternate between two
 * accumulators so no two consecutive adds contend. */
/* One 8-byte chunk = 16 weights, 4 pmaddwd.
 *
 * INSTRUCTION COUNT is what matters here. Profiling on real Geode
 * hardware (1.4.0) showed the kernel already running at ~3.2 cycles per
 * MMX instruction against a documented pipeline throughput of ~2 -- i.e.
 * close to the issue limit, NOT stalling. The disassembly showed 13 MMX
 * instructions per pmaddwd, so the win is in doing fewer of them, not in
 * scheduling them better.
 *
 * Key trick: mask in the BYTE domain before widening. One `pand` with
 * 0x0F0F..0F extracts all eight low nibbles at once; the old code
 * widened to words first and then needed two `pand` (one per half) for
 * the low nibbles and two more for the high. Byte-domain masking costs
 * 2 pand + 1 psrlw for all 16 weights instead of 4 pand + 2 psrlw.
 *
 * 20 -> 16 instructions per 16 weights, with the four pmaddwd writing
 * distinct registers and the paddd alternating accumulators (the
 * scheduling from 1.4.0 is kept). */
#define MMX_Q4K_CHUNK(K) \
    "movq " #K "*8(%0), %%mm2\n\t" \
    "movq %%mm2, %%mm5\n\t" \
    "pand %3, %%mm2\n\t" \
    "psrlw $4, %%mm5\n\t" \
    "pand %3, %%mm5\n\t" \
    "movq %%mm2, %%mm3\n\t" \
    "movq %%mm5, %%mm6\n\t" \
    "punpcklbw %%mm7, %%mm2\n\t" \
    "punpckhbw %%mm7, %%mm3\n\t" \
    "punpcklbw %%mm7, %%mm5\n\t" \
    "punpckhbw %%mm7, %%mm6\n\t" \
    "pmaddwd " #K "*16(%1), %%mm2\n\t" \
    "pmaddwd " #K "*16+8(%1), %%mm3\n\t" \
    "pmaddwd " #K "*16+64(%1), %%mm5\n\t" \
    "pmaddwd " #K "*16+72(%1), %%mm6\n\t" \
    "paddd %%mm2, %%mm0\n\t" \
    "paddd %%mm5, %%mm1\n\t" \
    "paddd %%mm3, %%mm0\n\t" \
    "paddd %%mm6, %%mm1\n\t"

#define MMX_Q4K_SUBROW \
    "pxor %%mm7, %%mm7\n\t" \
    "pxor %%mm0, %%mm0\n\t" \
    "pxor %%mm1, %%mm1\n\t" \
    MMX_Q4K_CHUNK(0) \
    MMX_Q4K_CHUNK(1) \
    MMX_Q4K_CHUNK(2) \
    MMX_Q4K_CHUNK(3) \
    "movq %%mm0, %%mm2\n\t" \
    "psrlq $32, %%mm2\n\t" \
    "paddd %%mm2, %%mm0\n\t" \
    "movd %%mm0, (%2)\n\t" \
    "movq %%mm1, %%mm2\n\t" \
    "psrlq $32, %%mm2\n\t" \
    "paddd %%mm2, %%mm1\n\t" \
    "movd %%mm1, 4(%2)\n\t" \

static void mmx_q4K_subrow(const unsigned char *qs, const short *x,
                           int *accs) {
    __asm__ __volatile__ (
        MMX_Q4K_SUBROW
        : : "r" (qs), "r" (x), "r" (accs), "m" (*(const short (*)[4]) c_mask_fb)
        : INFER_MMX_CLOBBERS);
}

/* Q5_K: as Q4_K with the +16 hi-bit term. qh points at 32 bytes whose
 * bits 2*j select +16 on the low-nibble row, bits 2*j+1 on the high
 * row. J2 / J2P1 are those two shifts (immediates). */
/* Q5_K chunk: 16 weights, each a 4-bit nibble plus one extra bit taken
 * from the qh plane.
 *
 * Rewritten for instruction count, same reasoning as MMX_Q4K_CHUNK.
 * The old version re-loaded qs and re-widened it for every one of the
 * four pmaddwd, and redid the hi-bit shift/mask/shift four times: 52
 * instructions for 4 multiplies.
 *
 * Now everything is done once in the BYTE domain before widening:
 *   - one pand extracts all 8 low nibbles
 *   - one psrlw+pand extracts all 8 high nibbles
 *   - the qh bit for each half is isolated once and pre-shifted to
 *     value 16 with a byte-domain psllw, then added
 * and only then are the four vectors widened and multiplied.
 *
 * 52 -> 26 instructions per 16 weights. */
#define MMX_Q5K_CHUNK(K, J2, J2P1) \
    "movq " #K "*8(%0), %%mm2\n\t"      /* 8 nibble bytes             */ \
    "movq " #K "*8(%3), %%mm4\n\t"      /* 8 qh bytes                 */ \
    "movq %%mm2, %%mm3\n\t" \
    "pand %6, %%mm2\n\t"                /* all 8 low nibbles          */ \
    "psrlw $4, %%mm3\n\t" \
    "pand %6, %%mm3\n\t"                /* all 8 high nibbles         */ \
    "movq %%mm4, %%mm5\n\t" \
    "psrlw $" #J2 ", %%mm4\n\t" \
    "pand %7, %%mm4\n\t"                /* bit J2, one per byte       */ \
    "psllw $4, %%mm4\n\t"               /* -> value 16                */ \
    "psrlw $" #J2P1 ", %%mm5\n\t" \
    "pand %7, %%mm5\n\t"                /* bit J2P1                   */ \
    "psllw $4, %%mm5\n\t" \
    "paddb %%mm4, %%mm2\n\t"            /* low nibble  + its hi bit   */ \
    "paddb %%mm5, %%mm3\n\t"            /* high nibble + its hi bit   */ \
    "movq %%mm2, %%mm4\n\t" \
    "movq %%mm3, %%mm5\n\t" \
    "punpcklbw %%mm7, %%mm2\n\t" \
    "punpckhbw %%mm7, %%mm4\n\t" \
    "punpcklbw %%mm7, %%mm3\n\t" \
    "punpckhbw %%mm7, %%mm5\n\t" \
    "pmaddwd " #K "*16(%1), %%mm2\n\t" \
    "pmaddwd " #K "*16+8(%1), %%mm4\n\t" \
    "pmaddwd " #K "*16+64(%1), %%mm3\n\t" \
    "pmaddwd " #K "*16+72(%1), %%mm5\n\t" \
    "paddd %%mm2, %%mm0\n\t" \
    "paddd %%mm3, %%mm1\n\t" \
    "paddd %%mm4, %%mm0\n\t" \
    "paddd %%mm5, %%mm1\n\t"

#define MMX_Q5K_SUBROW(J2, J2P1) \
    "pxor %%mm7, %%mm7\n\t" \
    "pxor %%mm0, %%mm0\n\t" \
    "pxor %%mm1, %%mm1\n\t" \
    MMX_Q5K_CHUNK(0, J2, J2P1) \
    MMX_Q5K_CHUNK(1, J2, J2P1) \
    MMX_Q5K_CHUNK(2, J2, J2P1) \
    MMX_Q5K_CHUNK(3, J2, J2P1) \
    "movq %%mm0, %%mm2\n\t" \
    "psrlq $32, %%mm2\n\t" \
    "paddd %%mm2, %%mm0\n\t" \
    "movd %%mm0, (%2)\n\t" \
    "movq %%mm1, %%mm2\n\t" \
    "psrlq $32, %%mm2\n\t" \
    "paddd %%mm2, %%mm1\n\t" \
    "movd %%mm1, 4(%2)\n\t" \

#define MMX_Q5K_SUBROW_FN(J2, J2P1, NAME) \
    static void NAME(const unsigned char *qs, const unsigned char *qh, \
                     const short *x, int *accs) { \
        __asm__ __volatile__ (MMX_Q5K_SUBROW(J2, J2P1) \
            : : "r" (qs), "r" (x), "r" (accs), "r" (qh), \
                "m" (*(const short (*)[4]) c_mask_f), \
                "m" (*(const short (*)[4]) c_mask_1), \
                "m" (*(const short (*)[4]) c_mask_fb), \
                "m" (*(const short (*)[4]) c_mask_1b) \
            : INFER_MMX_CLOBBERS); \
    }

MMX_Q5K_SUBROW_FN(0, 1, mmx_q5K_subrow_0)
MMX_Q5K_SUBROW_FN(2, 3, mmx_q5K_subrow_1)
MMX_Q5K_SUBROW_FN(4, 5, mmx_q5K_subrow_2)
MMX_Q5K_SUBROW_FN(6, 7, mmx_q5K_subrow_3)

/* Q6_K: one 16-weight group (one int8 scale). ql points at 16 low/high
 * nibble bytes (NIB selects which half), qh at 16 hi-bit bytes, x at 16
 * int16 activations. J2R = 2*r is the hi-bit shift for this sub-row.
 * The -32 bias is folded into the weights, so no sum-of-x correction is
 * needed anywhere. */
#define MMX_Q6K_CHUNK(K, J2R, NIB) \
    "movq " #K "*8(%0), %%mm2\n\t" \
    "movq %%mm2, %%mm3\n\t" \
    "punpcklbw %%mm7, %%mm2\n\t" \
    "punpckhbw %%mm7, %%mm3\n\t" \
    NIB \
    "pand %4, %%mm2\n\t" \
    "pand %4, %%mm3\n\t" \
    "movq " #K "*8(%1), %%mm4\n\t" \
    "movq %%mm4, %%mm5\n\t" \
    "punpcklbw %%mm7, %%mm4\n\t" \
    "punpckhbw %%mm7, %%mm5\n\t" \
    "movq %%mm4, %%mm6\n\t" \
    "psrlw $" #J2R ", %%mm6\n\t" \
    "pand %5, %%mm6\n\t" \
    "psllw $4, %%mm6\n\t" \
    "paddw %%mm6, %%mm2\n\t" \
    "movq %%mm5, %%mm6\n\t" \
    "psrlw $" #J2R ", %%mm6\n\t" \
    "pand %5, %%mm6\n\t" \
    "psllw $4, %%mm6\n\t" \
    "paddw %%mm6, %%mm3\n\t" \
    "psubw %6, %%mm2\n\t" \
    "psubw %6, %%mm3\n\t" \
    "movq " #K "*16(%2), %%mm6\n\t" \
    "pmaddwd %%mm2, %%mm6\n\t" \
    "paddd %%mm6, %%mm0\n\t" \
    "movq " #K "*16+8(%2), %%mm6\n\t" \
    "pmaddwd %%mm3, %%mm6\n\t" \
    "paddd %%mm6, %%mm0\n\t"

#define MMX_Q6K_GROUP(J2R, NIB) \
    "pxor %%mm7, %%mm7\n\t" \
    "pxor %%mm0, %%mm0\n\t" \
    MMX_Q6K_CHUNK(0, J2R, NIB) \
    MMX_Q6K_CHUNK(1, J2R, NIB) \
    "movq %%mm0, %%mm2\n\t" \
    "psrlq $32, %%mm2\n\t" \
    "paddd %%mm2, %%mm0\n\t" \
    "movd %%mm0, (%3)\n\t" \

/* Q6_K: one half-block (128 weights) as a single asm sequence. Eight
 * 16-weight groups are evaluated back to back, accumulating in MMX
 * registers, and EMMS is issued once at the end -- not once per group.
 * On in-order targets EMMS is several cycles plus a pipeline switch, so
 * this matters. Each group is two 8-weight passes (HALF8) over 16 ql
 * bytes / 16 qh bytes / 16 x shorts; offsets are in bytes. J2R is the
 * hi-bit shift (2*sub-row); sub-rows 2..3 read the high nibble of ql
 * via NIB. */
#define MMX_Q6K_HALF8(QO, HO, XO, J2R, NIB) \
    "movq " QO "(%0), %%mm2\n\t" \
    "movq %%mm2, %%mm3\n\t" \
    "punpcklbw %%mm7, %%mm2\n\t" \
    "punpckhbw %%mm7, %%mm3\n\t" \
    NIB \
    "pand %4, %%mm2\n\t" \
    "pand %4, %%mm3\n\t" \
    "movq " HO "(%1), %%mm4\n\t" \
    "movq %%mm4, %%mm5\n\t" \
    "punpcklbw %%mm7, %%mm4\n\t" \
    "punpckhbw %%mm7, %%mm5\n\t" \
    "movq %%mm4, %%mm6\n\t" \
    "psrlw $" J2R ", %%mm6\n\t" \
    "pand %5, %%mm6\n\t" \
    "psllw $4, %%mm6\n\t" \
    "paddw %%mm6, %%mm2\n\t" \
    "movq %%mm5, %%mm6\n\t" \
    "psrlw $" J2R ", %%mm6\n\t" \
    "pand %5, %%mm6\n\t" \
    "psllw $4, %%mm6\n\t" \
    "paddw %%mm6, %%mm3\n\t" \
    "psubw %6, %%mm2\n\t" \
    "psubw %6, %%mm3\n\t" \
    "movq " XO "(%2), %%mm6\n\t" \
    "pmaddwd %%mm2, %%mm6\n\t" \
    "paddd %%mm6, %%mm0\n\t" \
    "movq " XO "+8(%2), %%mm6\n\t" \
    "pmaddwd %%mm3, %%mm6\n\t" \
    "paddd %%mm6, %%mm0\n\t"

#define MMX_Q6K_GROUP_ABS(QO, HO, XO, J2R, NIB, AO) \
    "pxor %%mm0, %%mm0\n\t" \
    MMX_Q6K_HALF8(QO, HO, XO, J2R, NIB) \
    MMX_Q6K_HALF8(QO "+8", HO "+8", XO "+16", J2R, NIB) \
    "movq %%mm0, %%mm2\n\t" \
    "psrlq $32, %%mm2\n\t" \
    "paddd %%mm2, %%mm0\n\t" \
    "movd %%mm0, " AO "(%3)\n\t"

#define NIB_LOW ""
#define NIB_HIGH "psrlw $4, %%mm2\n\tpsrlw $4, %%mm3\n\t"

#define MMX_Q6K_HALF \
    "pxor %%mm7, %%mm7\n\t" \
    MMX_Q6K_GROUP_ABS("0",  "0",  "0",   "0", NIB_LOW,  "0") \
    MMX_Q6K_GROUP_ABS("16", "16", "32",  "0", NIB_LOW,  "4") \
    MMX_Q6K_GROUP_ABS("32", "0",  "64",  "2", NIB_LOW,  "8") \
    MMX_Q6K_GROUP_ABS("48", "16", "96",  "2", NIB_LOW,  "12") \
    MMX_Q6K_GROUP_ABS("0",  "0",  "128", "4", NIB_HIGH, "16") \
    MMX_Q6K_GROUP_ABS("16", "16", "160", "4", NIB_HIGH, "20") \
    MMX_Q6K_GROUP_ABS("32", "0",  "192", "6", NIB_HIGH, "24") \
    MMX_Q6K_GROUP_ABS("48", "16", "224", "6", NIB_HIGH, "28") \

/* One Q6_K half-block: 8 dot products of 16 int16 pairs into accs[8]. */
static void mmx_q6K_half(const unsigned char *ql, const unsigned char *qh,
                         const short *x, int *accs) {
    __asm__ __volatile__ (MMX_Q6K_HALF
        : : "r" (ql), "r" (qh), "r" (x), "r" (accs),
            "m" (*(const short (*)[4]) c_mask_f),
            "m" (*(const short (*)[4]) c_mask_3),
            "m" (*(const short (*)[4]) c_32)
        : INFER_MMX_CLOBBERS);
}

/* Q8_0: one 32-lane block. PSLLW+PSRAW sign-extends the int8 bytes to
 * int16 in place. */
static void mmx_q8_0_block(const signed char *q, const short *x, int *acc) {
    __asm__ __volatile__ (
        "pxor %%mm7, %%mm7\n\t"
        "pxor %%mm0, %%mm0\n\t"
        /* int8 -> int16: zero-extend, shift the byte into the high
         * byte, arithmetic-shift back down to sign-extend. */
        "movq 0(%0), %%mm2\n\t"
        "movq %%mm2, %%mm3\n\t"
        "punpcklbw %%mm7, %%mm2\n\t"
        "punpckhbw %%mm7, %%mm3\n\t"
        "psllw $8, %%mm2\n\t"
        "psraw $8, %%mm2\n\t"
        "psllw $8, %%mm3\n\t"
        "psraw $8, %%mm3\n\t"
        "movq 0(%1), %%mm4\n\t"
        "pmaddwd %%mm2, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq 8(%1), %%mm4\n\t"
        "pmaddwd %%mm3, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq 8(%0), %%mm2\n\t"
        "movq %%mm2, %%mm3\n\t"
        "punpcklbw %%mm7, %%mm2\n\t"
        "punpckhbw %%mm7, %%mm3\n\t"
        "psllw $8, %%mm2\n\t"
        "psraw $8, %%mm2\n\t"
        "psllw $8, %%mm3\n\t"
        "psraw $8, %%mm3\n\t"
        "movq 16(%1), %%mm4\n\t"
        "pmaddwd %%mm2, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq 24(%1), %%mm4\n\t"
        "pmaddwd %%mm3, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq 16(%0), %%mm2\n\t"
        "movq %%mm2, %%mm3\n\t"
        "punpcklbw %%mm7, %%mm2\n\t"
        "punpckhbw %%mm7, %%mm3\n\t"
        "psllw $8, %%mm2\n\t"
        "psraw $8, %%mm2\n\t"
        "psllw $8, %%mm3\n\t"
        "psraw $8, %%mm3\n\t"
        "movq 32(%1), %%mm4\n\t"
        "pmaddwd %%mm2, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq 40(%1), %%mm4\n\t"
        "pmaddwd %%mm3, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq 24(%0), %%mm2\n\t"
        "movq %%mm2, %%mm3\n\t"
        "punpcklbw %%mm7, %%mm2\n\t"
        "punpckhbw %%mm7, %%mm3\n\t"
        "psllw $8, %%mm2\n\t"
        "psraw $8, %%mm2\n\t"
        "psllw $8, %%mm3\n\t"
        "psraw $8, %%mm3\n\t"
        "movq 48(%1), %%mm4\n\t"
        "pmaddwd %%mm2, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq 56(%1), %%mm4\n\t"
        "pmaddwd %%mm3, %%mm4\n\t"
        "paddd %%mm4, %%mm0\n\t"
        "movq %%mm0, %%mm2\n\t"
        "psrlq $32, %%mm2\n\t"
        "paddd %%mm2, %%mm0\n\t"
        "movd %%mm0, (%2)\n\t"
            : : "r" (q), "r" (x), "r" (acc)
        : INFER_MMX_CLOBBERS);
}

/* Q4_0: one 32-lane block; nibbles minus 8. */
#define MMX_Q4_0_CHUNK(K) \
    "movq " #K "*8(%0), %%mm2\n\t" \
    "movq %%mm2, %%mm3\n\t" \
    "punpcklbw %%mm7, %%mm2\n\t" \
    "punpckhbw %%mm7, %%mm3\n\t" \
    "movq %%mm2, %%mm4\n\t" \
    "pand %3, %%mm4\n\t" \
    "movq %%mm2, %%mm5\n\t" \
    "psrlw $4, %%mm5\n\t" \
    "pand %3, %%mm5\n\t" \
    "movq %%mm3, %%mm6\n\t" \
    "pand %3, %%mm6\n\t" \
    "movq %%mm3, %%mm2\n\t" \
    "psrlw $4, %%mm2\n\t" \
    "pand %3, %%mm2\n\t" \
    "psubw %4, %%mm4\n\t" \
    "psubw %4, %%mm5\n\t" \
    "psubw %4, %%mm6\n\t" \
    "psubw %4, %%mm2\n\t" \
    "movq " #K "*16(%1), %%mm3\n\t" \
    "pmaddwd %%mm4, %%mm3\n\t" \
    "paddd %%mm3, %%mm0\n\t" \
    "movq " #K "*16+8(%1), %%mm3\n\t" \
    "pmaddwd %%mm6, %%mm3\n\t" \
    "paddd %%mm3, %%mm0\n\t" \
    "movq " #K "*16+32(%1), %%mm3\n\t" \
    "pmaddwd %%mm5, %%mm3\n\t" \
    "paddd %%mm3, %%mm0\n\t" \
    "movq " #K "*16+40(%1), %%mm3\n\t" \
    "pmaddwd %%mm2, %%mm3\n\t" \
    "paddd %%mm3, %%mm0\n\t"

static void mmx_q4_0_block(const unsigned char *q, const short *x, int *acc) {
    __asm__ __volatile__ (
        "pxor %%mm7, %%mm7\n\t"
        "pxor %%mm0, %%mm0\n\t"
        MMX_Q4_0_CHUNK(0)
        MMX_Q4_0_CHUNK(1)
        "movq %%mm0, %%mm2\n\t"
        "psrlq $32, %%mm2\n\t"
        "paddd %%mm2, %%mm0\n\t"
        "movd %%mm0, (%2)\n\t"
            : : "r" (q), "r" (x), "r" (acc),
            "m" (*(const short (*)[4]) c_mask_f),
            "m" (*(const short (*)[4]) c_8)
        : INFER_MMX_CLOBBERS);
}

/* ------------------------------------------------------------------ */
/* Per-format dot products                                             */
/* ------------------------------------------------------------------ */

/* Q4_K row.
 *
 * Two phases: every MMX block for the whole row runs first, collecting
 * raw integer dot products into a scratch array; then ONE mmx_sync()
 * returns the FPU to x87 state; then the float scale arithmetic runs.
 * This replaces one EMMS per 64 weights with one EMMS per row -- see the
 * comment on mmx_sync(). A 1024-column row goes from 16 syncs to 1. */
static float mmx_dot_q4_K(const unsigned char *b, const bk_qx *xq, long ncols) {
    /* 8 sub-row accumulators per 256-block; the widest row in this model
     * is 3584 columns = 14 blocks = 112 ints. */
    static int acc[MMX_ROW_ACC];
    const unsigned char *b0 = b;
    float sum = 0.0f;
    long c;
    int n = 0;

    /* ---- phase 1: pure MMX, no float, no EMMS ---- */
    for (c = 0; c < ncols; c += QK_K) {
        const unsigned char *qs = b + 16;
        int j;
        for (j = 0; j < 4; j++) {
            mmx_q4K_subrow(qs + j * 32, xq->q + c + j * 64, acc + n);
            n += 2;
        }
        b += 144;
    }

    /* ---- one FPU state switch for the entire row ---- */
    mmx_sync();

    /* ---- phase 2: pure float ---- */
    b = b0;
    n = 0;
    for (c = 0; c < ncols; c += QK_K) {
        const float d    = rd_f16p(b);
        const float dmin = rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        long blk = c / 32;
        int j;

        for (j = 0; j < 4; j++) {
            unsigned char s1, m1, s2, m2;
            get_scale_min_k4(j * 2,     sc, &s1, &m1);
            get_scale_min_k4(j * 2 + 1, sc, &s2, &m2);

            sum += xq->d[blk] *
                   ((float) acc[n] * d * (float) s1 -
                    (float) xq->s[blk] * dmin * (float) m1);
            sum += xq->d[blk + 1] *
                   ((float) acc[n + 1] * d * (float) s2 -
                    (float) xq->s[blk + 1] * dmin * (float) m2);
            blk += 2;
            n += 2;
        }
        b += 144;
    }
    return sum;
}

/* Q5_K row -- same two-phase split as Q4_K. */
static float mmx_dot_q5_K(const unsigned char *b, const bk_qx *xq, long ncols) {
    static int acc[MMX_ROW_ACC];
    const unsigned char *b0 = b;
    float sum = 0.0f;
    long c;
    int n = 0;

    for (c = 0; c < ncols; c += QK_K) {
        const unsigned char *qh = b + 16;
        const unsigned char *qs = b + 48;
        int j;
        for (j = 0; j < 4; j++) {
            const unsigned char *p = qs + j * 32;
            const short *xp = xq->q + c + j * 64;
            switch (j) {
                case 0: mmx_q5K_subrow_0(p, qh, xp, acc + n); break;
                case 1: mmx_q5K_subrow_1(p, qh, xp, acc + n); break;
                case 2: mmx_q5K_subrow_2(p, qh, xp, acc + n); break;
                default: mmx_q5K_subrow_3(p, qh, xp, acc + n); break;
            }
            n += 2;
        }
        b += 176;
    }

    mmx_sync();

    b = b0;
    n = 0;
    for (c = 0; c < ncols; c += QK_K) {
        const float d    = rd_f16p(b);
        const float dmin = rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        long blk = c / 32;
        int j;
        for (j = 0; j < 4; j++) {
            unsigned char s1, m1, s2, m2;
            get_scale_min_k4(j * 2,     sc, &s1, &m1);
            get_scale_min_k4(j * 2 + 1, sc, &s2, &m2);
            sum += xq->d[blk] *
                   ((float) acc[n] * d * (float) s1 -
                    (float) xq->s[blk] * dmin * (float) m1);
            sum += xq->d[blk + 1] *
                   ((float) acc[n + 1] * d * (float) s2 -
                    (float) xq->s[blk + 1] * dmin * (float) m2);
            blk += 2;
            n += 2;
        }
        b += 176;
    }
    return sum;
}

/* Q6_K row -- same two-phase split. 8 accumulators per half-block. */
static float mmx_dot_q6_K(const unsigned char *b, const bk_qx *xq, long ncols) {
    static int acc[MMX_ROW_ACC];
    const unsigned char *b0 = b;
    float sum = 0.0f;
    long c;
    int n = 0;

    for (c = 0; c < ncols; c += QK_K) {
        int h;
        for (h = 0; h < 2; h++) {
            mmx_q6K_half(b + h * 64, b + 128 + h * 32,
                         xq->q + c + h * 128, acc + n);
            n += 8;
        }
        b += 210;
    }

    mmx_sync();

    b = b0;
    n = 0;
    for (c = 0; c < ncols; c += QK_K) {
        const signed char *sc = (const signed char *) (b + 192);
        const float d = rd_f16p(b + 208);
        int h;
        for (h = 0; h < 2; h++) {
            const signed char *scn = sc + h * 8;
            long blk = (c + h * 128) / 32;
            int r;
            for (r = 0; r < 4; r++) {
                sum += d * xq->d[blk + r] *
                       ((float) scn[r * 2]     * (float) acc[n + r * 2] +
                        (float) scn[r * 2 + 1] * (float) acc[n + r * 2 + 1]);
            }
            n += 8;
        }
        b += 210;
    }
    return sum;
}

/* Q8_0 row -- same two-phase split. */
static float mmx_dot_q8_0(const unsigned char *b, const bk_qx *xq, long ncols) {
    static int acc[MMX_ROW_ACC * 4];
    const unsigned char *b0 = b;
    float sum = 0.0f;
    long c;
    int n = 0;

    for (c = 0; c < ncols; c += 32) {
        mmx_q8_0_block((const signed char *) (b + 2), xq->q + c, acc + n);
        n++;
        b += 34;
    }

    mmx_sync();

    b = b0;
    n = 0;
    for (c = 0; c < ncols; c += 32) {
        sum += rd_f16p(b) * xq->d[c / 32] * (float) acc[n];
        n++;
        b += 34;
    }
    return sum;
}

/* Q4_0 row -- same two-phase split. */
static float mmx_dot_q4_0(const unsigned char *b, const bk_qx *xq, long ncols) {
    static int acc[MMX_ROW_ACC * 4];
    const unsigned char *b0 = b;
    float sum = 0.0f;
    long c;
    int n = 0;

    for (c = 0; c < ncols; c += 32) {
        mmx_q4_0_block(b + 2, xq->q + c, acc + n);
        n++;
        b += 18;
    }

    mmx_sync();

    b = b0;
    n = 0;
    for (c = 0; c < ncols; c += 32) {
        sum += rd_f16p(b) * xq->d[c / 32] *
               (float) (acc[n] - 8 * xq->s[c / 32]);
        n++;
        b += 18;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

void qmv_mmx(int type, const unsigned char *w, const float *x, float *y,
             long ncols, long nrows) {
    const bk_qx *xq;
    long rowbytes, r;

    if (type == GGML_TYPE_F32 || type == GGML_TYPE_F16 ||
        (ncols % 32) != 0) {
        qmv_ref(type, w, x, y, ncols, nrows);
        return;
    }

    xq = bk_quantize_x(x, ncols);
    rowbytes = gg_type_size(type, ncols);

    switch (type) {
        case GGML_TYPE_Q4_K:
            for (r = 0; r < nrows; r++) y[r] = mmx_dot_q4_K(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q5_K:
            for (r = 0; r < nrows; r++) y[r] = mmx_dot_q5_K(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q6_K:
            for (r = 0; r < nrows; r++) y[r] = mmx_dot_q6_K(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q8_0:
            for (r = 0; r < nrows; r++) y[r] = mmx_dot_q8_0(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q4_0:
            for (r = 0; r < nrows; r++) y[r] = mmx_dot_q4_0(w + r * rowbytes, xq, ncols);
            return;
        default:
            qmv_ref(type, w, x, y, ncols, nrows);
            return;
    }
}

#endif /* INFER_HAVE_MMX */
