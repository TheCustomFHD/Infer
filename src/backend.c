/* backend.c -- selectable matrix-vector kernels and CPU detection.
 *
 * Profiling the reference build shows ~95% of inference time inside
 * q_matvec. This file provides faster implementations of exactly that
 * operation and the machinery to pick one.
 *
 * The `i8` backend is the important one. The reference kernel does, per
 * weight, one 4-bit unpack, one int->float conversion and one float
 * multiply-add. On an in-order CPU with a slow FPU (a 486, or a Geode
 * LX whose FADD has 7-cycle latency) the float work dominates.
 *
 * `i8` instead quantises the activation vector once per matrix into
 * 32-element blocks of int8 with a per-block scale -- exactly the
 * scheme GGML uses for its own activation quantisation -- and then
 * evaluates each dot product entirely in integer arithmetic. The block
 * scales are folded in once per 32 weights rather than once per weight,
 * so the float op count drops by ~32x and the inner loop becomes a
 * plain integer multiply-accumulate.
 *
 * Accuracy is unaffected in practice: the activations are quantised to
 * the same precision GGML uses, and the weights are not touched at all.
 * tests/t_backend.c measures the difference against the reference.
 *
 * ANSI C (C89) throughout. CPU detection uses GNU inline assembly but
 * is compiled only on x86 GCC/Clang and degrades to "no features"
 * everywhere else.
 */

#include "infer.h"
#include "backend.h"
#include "tpool.h"
#include "prof.h"
#include "sys.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CPU feature detection                                               */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define BK_X86_CPUID 1
#endif

static int   cpu_probed = 0;
static int   cpu_mmx    = 0;
static int   cpu_3dnow  = 0;
static int   cpu_cmov   = 0;
static int   cpu_sse2   = 0;
static int   cpu_avx2   = 0;
static char  cpu_vendor_s[16] = "unknown";

#ifdef BK_X86_CPUID
static void bk_cpuid(unsigned long leaf, unsigned long *a, unsigned long *b,
                     unsigned long *c, unsigned long *d) {
    unsigned long ra, rb, rc, rd;
#if defined(__i386__) && defined(__PIC__)
    /* %ebx is the PIC register on 32-bit; save and restore it. */
    __asm__ __volatile__ ("xchgl %%ebx, %1\n\t"
                          "cpuid\n\t"
                          "xchgl %%ebx, %1"
                          : "=a" (ra), "=r" (rb), "=c" (rc), "=d" (rd)
                          : "0" (leaf), "2" (0));
#else
    __asm__ __volatile__ ("cpuid"
                          : "=a" (ra), "=b" (rb), "=c" (rc), "=d" (rd)
                          : "0" (leaf), "2" (0));
#endif
    *a = ra; *b = rb; *c = rc; *d = rd;
}

/* CPUID itself is absent on a plain 486: it exists only if bit 21 of
 * EFLAGS (ID) can be toggled. Check that before executing it. */
static int bk_has_cpuid(void) {
#if defined(__x86_64__)
    return 1;
#else
    int res;
    __asm__ __volatile__ (
        "pushfl\n\t"
        "pushfl\n\t"
        "popl %%eax\n\t"
        "movl %%eax, %%ecx\n\t"
        "xorl $0x200000, %%eax\n\t"
        "pushl %%eax\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %%eax\n\t"
        "xorl %%ecx, %%eax\n\t"
        "andl $0x200000, %%eax\n\t"
        "popfl"
        : "=a" (res) : : "ecx", "cc");
    return res != 0;
#endif
}

/* Read XCR0. AVX adds architectural state (the upper half of ymm), so
 * the CPU feature bit alone is not enough: the OS must also have
 * promised to save and restore it across a context switch. If it has
 * not, the CPU raises #UD on the first VEX-encoded instruction.
 *
 * This is not hypothetical for this project. Windows XP never gained
 * AVX support, so an XP machine on late silicon reports AVX2 in CPUID
 * and still cannot run it. Checking OSXSAVE + XCR0 is the difference
 * between selecting another backend and crashing.
 *
 * Only called once OSXSAVE (CPUID.1:ECX bit 27) is known set, so
 * XGETBV itself is safe to execute here. */
static unsigned long bk_xgetbv0(void) {
    unsigned long eax, edx;
    __asm__ __volatile__ (".byte 0x0f, 0x01, 0xd0"   /* xgetbv */
                          : "=a" (eax), "=d" (edx) : "c" (0));
    (void) edx;
    return eax;
}
#endif /* BK_X86_CPUID */

static void probe_cpu(void) {
    if (cpu_probed) return;
    cpu_probed = 1;

#ifdef BK_X86_CPUID
    if (!bk_has_cpuid()) return;   /* 486 without CPUID: no features */

    {
        unsigned long a, b, c, d;
        bk_cpuid(0, &a, &b, &c, &d);

        memcpy(cpu_vendor_s + 0, &b, 4);
        memcpy(cpu_vendor_s + 4, &d, 4);
        memcpy(cpu_vendor_s + 8, &c, 4);
        cpu_vendor_s[12] = '\0';

        if (a >= 1) {
            unsigned long maxleaf = a;
            unsigned long c1, d1;

            bk_cpuid(1, &a, &b, &c, &d);
            c1 = c; d1 = d;
            cpu_cmov = (d1 & (1UL << 15)) ? 1 : 0;
            cpu_mmx  = (d1 & (1UL << 23)) ? 1 : 0;
            cpu_sse2 = (d1 & (1UL << 26)) ? 1 : 0;

            /* AVX2 needs four things to all agree:
             *   CPUID.1:ECX.OSXSAVE (27)  OS enabled XSAVE
             *   CPUID.1:ECX.AVX     (28)  CPU has AVX
             *   XCR0 bits 1 and 2         OS saves xmm AND ymm
             *   CPUID.7:EBX.AVX2    (5)   CPU has AVX2
             * Miss the XCR0 check and an XP box with a Haswell chip
             * takes #UD instead of falling back. */
            if ((c1 & (1UL << 27)) && (c1 & (1UL << 28)) && maxleaf >= 7) {
                if ((bk_xgetbv0() & 0x6UL) == 0x6UL) {
                    bk_cpuid(7, &a, &b, &c, &d);
                    cpu_avx2 = (b & (1UL << 5)) ? 1 : 0;
                }
            }
        }

        /* 3DNow! lives in the AMD extended leaves. */
        bk_cpuid(0x80000000UL, &a, &b, &c, &d);
        if (a >= 0x80000001UL) {
            bk_cpuid(0x80000001UL, &a, &b, &c, &d);
            cpu_3dnow = (d & (1UL << 31)) ? 1 : 0;
            if (!cpu_mmx && (d & (1UL << 23))) cpu_mmx = 1;
        }
    }
#endif
}

int bk_cpu_has_mmx(void)   { probe_cpu(); return cpu_mmx; }
int bk_cpu_has_3dnow(void) { probe_cpu(); return cpu_3dnow; }
int bk_cpu_has_cmov(void)  { probe_cpu(); return cpu_cmov; }
int bk_cpu_has_sse2(void)  { probe_cpu(); return cpu_sse2; }
int bk_cpu_has_avx2(void)  { probe_cpu(); return cpu_avx2; }
const char *bk_cpu_vendor(void) { probe_cpu(); return cpu_vendor_s; }


/* ------------------------------------------------------------------ */
/* Activation quantisation                                             */
/*                                                                     */
/* x is split into blocks of 32. Each block gets a scale d and 32       */
/* int8 values stored widened to int16 (so both the scalar and the MMX  */
/* kernels can read them without further unpacking). The sum of the     */
/* quantised values is kept too: the k-quant formats subtract a per-    */
/* block minimum, and that term needs sum(x) over the same block.       */
/* ------------------------------------------------------------------ */

static bk_qx  xq_buf;
static long   xq_cap = 0;

/* Explicit "already quantised" latch, NOT a content heuristic.
 *
 * Under the thread pool q_matvec quantises x once and then every
 * worker calls bk_quantize_x again for the same vector. Racing writes
 * to one shared buffer would be a genuine data race, so the repeats
 * must become no-ops.
 *
 * Guessing whether x changed (pointer + a couple of sampled values)
 * was the first attempt and it is unsafe: x lives in a scratch buffer
 * that is rewritten in place, so a false hit silently computes with
 * stale activations -- a wrong-answer bug that no test would reliably
 * catch. Instead the caller states it explicitly: bk_quantize_hold()
 * before the parallel region, bk_quantize_release() after. Outside
 * that window every call recomputes exactly as before. */
static int xq_held = 0;


/* Build the Q8_K-style plane: one scale per 256 activations, plus
 * per-16 sums. Only the AVX2 k-quant kernels need it, so it is built
 * on first request and reused until the next bk_quantize_x.
 *
 * Why a second plane at all: those kernels fold the weight scale into
 * an int32 accumulator that spans a whole 256-weight superblock and
 * convert to float once. That is only valid if the activation scale is
 * constant across the superblock, which the per-32 plane is not. */
const bk_qx *bk_quantize_k(const float *x, long n) {
    long nk, k, j;

    /* Callable without bk_quantize_x having run: the AVX2 k-quant
     * path uses this plane alone, so it owns the allocation and the
     * validity bookkeeping. */
    /* Only the hold latch may short-circuit this. A "has x changed?"
     * guess based on the pointer is unsafe: x lives in a scratch
     * buffer rewritten in place, so a false hit computes with stale
     * activations -- a silent wrong answer. The latch is set
     * explicitly by q_matvec around a parallel region and nowhere
     * else. */
    if (xq_held) return &xq_buf;

    if (n > xq_cap) {
        xq_cap    = n + 256;
        xq_buf.q  = (short *) xrealloc(xq_buf.q, sizeof(short) * (size_t) xq_cap);
        xq_buf.q8 = (signed char *) xrealloc(xq_buf.q8,
                                             sizeof(signed char) * (size_t) xq_cap);
        xq_buf.k8 = (signed char *) xrealloc(xq_buf.k8,
                                             sizeof(signed char) * (size_t) xq_cap);
        xq_buf.d  = (float *) xrealloc(xq_buf.d, sizeof(float) *
                                       (size_t) (xq_cap / 32 + 2));
        xq_buf.s  = (int *)   xrealloc(xq_buf.s, sizeof(int) *
                                       (size_t) (xq_cap / 32 + 2));
        xq_buf.s16 = (int *)  xrealloc(xq_buf.s16, sizeof(int) *
                                       (size_t) (xq_cap / 16 + 2));
        xq_buf.kd = (float *) xrealloc(xq_buf.kd, sizeof(float) *
                                       (size_t) (xq_cap / 256 + 2));
        xq_buf.ks16 = (int *) xrealloc(xq_buf.ks16, sizeof(int) *
                                       (size_t) (xq_cap / 16 + 2));
        xq_buf.n = xq_cap;
    }

    nk = (n + 255) / 256;
    for (k = 0; k < nk; k++) {
        long j0 = k * 256, j1 = j0 + 256;
        float amax = 0.0f, kd, kid;
        if (j1 > n) j1 = n;

        for (j = 0; j < 16; j++) xq_buf.ks16[k * 16 + j] = 0;

        /* The vectorised body lives in backend_avx2.c, the only object
         * compiled with -mavx2. This file is compiled at the i486
         * baseline so that ONE binary runs everywhere, so it may not
         * contain AVX2 itself -- it calls through a runtime-gated
         * pointer instead. bk_a2_* return 0 when unavailable. */
        amax = 0.0f;
        if (!bk_a2_amax(x + j0, j1 - j0, &amax)) {
            for (j = j0; j < j1; j++) {
                float a = x[j] < 0.0f ? -x[j] : x[j];
                if (a > amax) amax = a;
            }
        }
        if (amax == 0.0f) {
            for (j = j0; j < j0 + 256 && j < xq_cap; j++) xq_buf.k8[j] = 0;
            xq_buf.kd[k] = 0.0f;
            continue;
        }
        kd  = amax / 127.0f;
        kid = 127.0f / amax;
        xq_buf.kd[k] = kd;
/* Vectorised in backend_avx2.c (see above); scalar fallback here. */
        if (!bk_a2_quant256(x + j0, j1 - j0, kid,
                            xq_buf.k8 + j0, xq_buf.ks16 + k * 16)) {
            for (j = j0; j < j1; j++) {
                float v = x[j] * kid;
                int   q = (int) (v + (v >= 0.0f ? 0.5f : -0.5f));
                if (q >  127) q =  127;
                if (q < -127) q = -127;
                xq_buf.k8[j] = (signed char) q;
                xq_buf.ks16[k * 16 + (j - j0) / 16] += q;
            }
        }
        for (j = j1; j < j0 + 256 && j < xq_cap; j++) xq_buf.k8[j] = 0;
    }
    return &xq_buf;
}

void bk_quantize_hold(void)    { xq_held = 1; }
void bk_quantize_release(void) { xq_held = 0; }

const bk_qx *bk_quantize_x(const float *x, long n) {
    long nb = (n + 31) / 32;
    long i, b;

    if (xq_held) return &xq_buf;


    if (n > xq_cap) {
        xq_cap    = n + 256;
        xq_buf.q  = (short *) xrealloc(xq_buf.q, sizeof(short) * (size_t) xq_cap);
        /* Second copy of the same values as int8. VPMADDUBSW takes an
         * s8 operand directly; widening to int16 first would halve the
         * lane count and throw away the whole point of the
         * instruction. 1 byte per activation, so ~4 KB for the widest
         * row in this model -- cheap next to the weights. */
        xq_buf.q8 = (signed char *) xrealloc(xq_buf.q8,
                                             sizeof(signed char) * (size_t) xq_cap);
        xq_buf.k8 = (signed char *) xrealloc(xq_buf.k8,
                                             sizeof(signed char) * (size_t) xq_cap);
        xq_buf.kd = (float *) xrealloc(xq_buf.kd, sizeof(float) *
                                       (size_t) (xq_cap / 256 + 2));
        xq_buf.ks16 = (int *) xrealloc(xq_buf.ks16, sizeof(int) *
                                       (size_t) (xq_cap / 16 + 2));
        xq_buf.d  = (float *) xrealloc(xq_buf.d, sizeof(float) *
                                       (size_t) (xq_cap / 32 + 2));
        xq_buf.s  = (int *)   xrealloc(xq_buf.s, sizeof(int) *
                                       (size_t) (xq_cap / 32 + 2));
        xq_buf.s16 = (int *)  xrealloc(xq_buf.s16, sizeof(int) *
                                       (size_t) (xq_cap / 16 + 2));
    }
    xq_buf.n = n;

    for (b = 0; b < nb; b++) {
        long i0 = b * 32;
        long i1 = i0 + 32;
        float amax = 0.0f;
        float d, id;
        int sum = 0;
        int sum16_0 = 0, sum16_1 = 0;

        if (i1 > n) i1 = n;

        for (i = i0; i < i1; i++) {
            float a = x[i] < 0.0f ? -x[i] : x[i];
            if (a > amax) amax = a;
        }

        if (amax == 0.0f) {
            for (i = i0; i < i1; i++) { xq_buf.q[i] = 0; xq_buf.q8[i] = 0; }
            for (; i < i0 + 32; i++)  { xq_buf.q[i] = 0; xq_buf.q8[i] = 0; }
            xq_buf.d[b] = 0.0f;
            xq_buf.s[b] = 0;
            xq_buf.s16[2 * b]     = 0;
            xq_buf.s16[2 * b + 1] = 0;
            continue;
        }

        d  = amax / 127.0f;
        id = 127.0f / amax;

        for (i = i0; i < i1; i++) {
            /* round-half-away-from-zero without needing C99 rintf() */
            float v = x[i] * id;
            int   q = (int) (v + (v >= 0.0f ? 0.5f : -0.5f));
            if (q >  127) q =  127;
            if (q < -127) q = -127;
            xq_buf.q[i]  = (short) q;
            xq_buf.q8[i] = (signed char) q;
            sum += q;
            if (i - i0 < 16) sum16_0 += q;
            else             sum16_1 += q;
        }
        /* zero-pad a short tail so kernels can always read 32 */
        for (i = i1; i < i0 + 32; i++) {
            xq_buf.q[i]  = 0;
            xq_buf.q8[i] = 0;
        }

        xq_buf.d[b] = d;
        xq_buf.s[b] = sum;
        xq_buf.s16[2 * b]     = sum16_0;
        xq_buf.s16[2 * b + 1] = sum16_1;
    }

    /* The Q8_K-style plane (one scale per 256) is NOT built here; the
     * AVX2 k-quant kernels call bk_quantize_k() for it instead. Which
     * plane gets built is a property of the KERNEL, not of the caller,
     * so each builds only what it reads. */

    return &xq_buf;
}

void bk_free_scratch(void) {
    if (xq_buf.q) free(xq_buf.q);
    if (xq_buf.d) free(xq_buf.d);
    if (xq_buf.s) free(xq_buf.s);
    if (xq_buf.q8)  free(xq_buf.q8);
    if (xq_buf.k8)  free(xq_buf.k8);
    if (xq_buf.kd)  free(xq_buf.kd);
    if (xq_buf.ks16) free(xq_buf.ks16);
    if (xq_buf.s16) free(xq_buf.s16);
    xq_buf.q = NULL;
    xq_buf.d = NULL;
    xq_buf.s = NULL;
    xq_buf.q8  = NULL;
    xq_buf.k8  = NULL;
    xq_buf.kd  = NULL;
    xq_buf.ks16 = NULL;
    xq_held    = 0;
    xq_buf.s16 = NULL;
    xq_cap = 0;
}

/* ------------------------------------------------------------------ */
/* Shared k-quant helpers                                              */
/* ------------------------------------------------------------------ */

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
/* i8 backend: integer dot products                                    */
/* ------------------------------------------------------------------ */

static float i8_dot_q4_K(const unsigned char *b, const bk_qx *xq, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = rd_f16p(b);
        const float dmin = rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qs = b + 16;
        long blk = c / 32;
        int j;

        for (j = 0; j < 4; j++) {
            unsigned char s1, m1, s2, m2;
            const short *x1 = xq->q + c + j * 64;
            const short *x2 = x1 + 32;
            int acc1 = 0, acc2 = 0;
            int l;

            get_scale_min_k4(j * 2,     sc, &s1, &m1);
            get_scale_min_k4(j * 2 + 1, sc, &s2, &m2);

            /* Split the two nibble streams into separate loops.
             * Interleaved, each iteration has two independent chains
             * competing for the same load slot; separated, each loop
             * is a clean reduction the compiler can unroll and (where
             * the target allows) vectorise. (Finding 31.) */
            for (l = 0; l < 32; l++) acc1 += (qs[l] & 0xF) * x1[l];
            for (l = 0; l < 32; l++) acc2 += (qs[l] >> 4)  * x2[l];

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

static float i8_dot_q5_K(const unsigned char *b, const bk_qx *xq, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const float d    = rd_f16p(b);
        const float dmin = rd_f16p(b + 2);
        const unsigned char *sc = b + 4;
        const unsigned char *qh = b + 16;
        const unsigned char *qs = b + 48;
        long blk = c / 32;
        int j;

        for (j = 0; j < 4; j++) {
            unsigned char s1, m1, s2, m2;
            const short *x1 = xq->q + c + j * 64;
            const short *x2 = x1 + 32;
            int acc1 = 0, acc2 = 0;
            int sh1 = j * 2, sh2 = j * 2 + 1;
            int l;

            get_scale_min_k4(j * 2,     sc, &s1, &m1);
            get_scale_min_k4(j * 2 + 1, sc, &s2, &m2);

            /* branch-free hi-bit term: (h >> sh) & 1, shifted to 16.
             * A per-lane branch costs a pipeline flush on a 486.
             *
             * The nibbles are computed, not looked up. A table lookup
             * is a gather, which no vectoriser can handle, and it was
             * measured slower even scalar. (Finding 32.)
             *
             * Two shapes, and which one wins depends on whether the
             * compiler may vectorise. Interleaved, one loop reads both
             * planes once. Split, each loop is a clean reduction the
             * vectoriser can take, but both planes get re-read.
             *
             *   scalar (i486 165 -> 170 instrs, sparc64 147 -> 160):
             *       interleaved wins
             *   SSE2 (Q5_K matvec 3.489s -> 1.853s, 1.88x):
             *       split wins, by a lot
             *
             * Same reassociation either way -- proven integer-exact
             * over 200000 random sub-rows on x86-64, i486, SSE2 and
             * big-endian sparc64. (Finding 35.) */
#if defined(__SSE2__) || defined(__AVX2__)
            for (l = 0; l < 32; l++) {
                acc1 += ((int) (qs[l] & 0x0F) +
                         (((qh[l] >> sh1) & 1) << 4)) * x1[l];
            }
            for (l = 0; l < 32; l++) {
                acc2 += ((int) (qs[l] >> 4) +
                         (((qh[l] >> sh2) & 1) << 4)) * x2[l];
            }
#else
            for (l = 0; l < 32; l++) {
                unsigned char v = qs[l];
                int h = qh[l];
                int w1 = (int) (v & 0x0F) + (((h >> sh1) & 1) << 4);
                int w2 = (int) (v >> 4)   + (((h >> sh2) & 1) << 4);
                acc1 += w1 * x1[l];
                acc2 += w2 * x2[l];
            }
#endif

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

/* Sum of the quantised activations for a 16-wide group, precomputed in
 * bk_quantize_x (Q6_K weights are biased by -32; the bias term needs
 * this partial sum). */
static int sum16(const bk_qx *xq, long idx) {
    return xq->s16[idx / 16];
}

static float i8_dot_q6_K(const unsigned char *b, const bk_qx *xq, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += QK_K) {
        const unsigned char *ql = b;
        const unsigned char *qh = b + 128;
        const signed char   *sc = (const signed char *) (b + 192);
        const float d = rd_f16p(b + 208);
        int n;

        /* The four sub-rows are unrolled rather than selected inside the
         * inner loop: a switch on the row index there costs more than
         * the arithmetic it guards. Each sub-row splits into two groups
         * of 16 with their own int8 scale. */
        for (n = 0; n < 2; n++) {
            const unsigned char *qln = ql + n * 64;
            const unsigned char *qhn = qh + n * 32;
            const signed char   *scn = sc + n * 8;
            long off = c + n * 128;
            const short *xp = xq->q + off;
            long blk = off / 32;
            int a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            int a4 = 0, a5 = 0, a6 = 0, a7 = 0;
            int l;

            for (l = 0; l < 16; l++) {
                int h = qhn[l];
                a0 += (int) ((qln[l]      & 0xF) | (((h >> 0) & 3) << 4)) * xp[l];
                a2 += (int) ((qln[l + 32] & 0xF) | (((h >> 2) & 3) << 4)) * xp[l + 32];
                a4 += (int) ((qln[l]      >> 4)  | (((h >> 4) & 3) << 4)) * xp[l + 64];
                a6 += (int) ((qln[l + 32] >> 4)  | (((h >> 6) & 3) << 4)) * xp[l + 96];
            }
            for (l = 16; l < 32; l++) {
                int h = qhn[l];
                a1 += (int) ((qln[l]      & 0xF) | (((h >> 0) & 3) << 4)) * xp[l];
                a3 += (int) ((qln[l + 32] & 0xF) | (((h >> 2) & 3) << 4)) * xp[l + 32];
                a5 += (int) ((qln[l]      >> 4)  | (((h >> 4) & 3) << 4)) * xp[l + 64];
                a7 += (int) ((qln[l + 32] >> 4)  | (((h >> 6) & 3) << 4)) * xp[l + 96];
            }

            /* Q6_K stores q-32; fold the bias in via the block sum. */
            sum += d * xq->d[blk] *
                   ((float) scn[0] * (float) (a0 - 32 * sum16(xq, off +  0)) +
                    (float) scn[1] * (float) (a1 - 32 * sum16(xq, off + 16)));
            sum += d * xq->d[blk + 1] *
                   ((float) scn[2] * (float) (a2 - 32 * sum16(xq, off + 32)) +
                    (float) scn[3] * (float) (a3 - 32 * sum16(xq, off + 48)));
            sum += d * xq->d[blk + 2] *
                   ((float) scn[4] * (float) (a4 - 32 * sum16(xq, off + 64)) +
                    (float) scn[5] * (float) (a5 - 32 * sum16(xq, off + 80)));
            sum += d * xq->d[blk + 3] *
                   ((float) scn[6] * (float) (a6 - 32 * sum16(xq, off + 96)) +
                    (float) scn[7] * (float) (a7 - 32 * sum16(xq, off + 112)));
        }
        b += 210;
    }
    return sum;
}

static float i8_dot_q8_0(const unsigned char *b, const bk_qx *xq, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += 32) {
        const float d = rd_f16p(b);
        const signed char *q = (const signed char *) (b + 2);
        const short *xp = xq->q + c;
        int acc = 0;
        int l;

        for (l = 0; l < 32; l++) acc += (int) q[l] * xp[l];
        sum += d * xq->d[c / 32] * (float) acc;
        b += 34;
    }
    return sum;
}

static float i8_dot_q4_0(const unsigned char *b, const bk_qx *xq, long ncols) {
    float sum = 0.0f;
    long c;

    for (c = 0; c < ncols; c += 32) {
        const float d = rd_f16p(b);
        const unsigned char *q = b + 2;
        const short *xp = xq->q + c;
        int acc = 0;
        int l;

        for (l = 0; l < 16; l++) {
            acc += (int) (q[l] & 0xF) * xp[l];
            acc += (int) (q[l] >> 4)  * xp[l + 16];
        }
        /* value = d * (nibble - 8) */
        sum += d * xq->d[c / 32] * (float) (acc - 8 * xq->s[c / 32]);
        b += 18;
    }
    return sum;
}

void qmv_i8(int type, const unsigned char *w, const float *x, float *y,
            long ncols, long nrows) {
    const bk_qx *xq;
    long rowbytes, r;

    /* Types without a block structure gain nothing here. */
    if (type == GGML_TYPE_F32 || type == GGML_TYPE_F16 ||
        (ncols % 32) != 0) {
        qmv_ref(type, w, x, y, ncols, nrows);
        return;
    }

    xq = bk_quantize_x(x, ncols);
    rowbytes = gg_type_size(type, ncols);

    switch (type) {
        case GGML_TYPE_Q4_K:
            for (r = 0; r < nrows; r++) y[r] = i8_dot_q4_K(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q5_K:
            for (r = 0; r < nrows; r++) y[r] = i8_dot_q5_K(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q6_K:
            for (r = 0; r < nrows; r++) y[r] = i8_dot_q6_K(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q8_0:
            for (r = 0; r < nrows; r++) y[r] = i8_dot_q8_0(w + r * rowbytes, xq, ncols);
            return;
        case GGML_TYPE_Q4_0:
            for (r = 0; r < nrows; r++) y[r] = i8_dot_q4_0(w + r * rowbytes, xq, ncols);
            return;
        default:
            qmv_ref(type, w, x, y, ncols, nrows);
            return;
    }
}

/* ------------------------------------------------------------------ */
/* Backend table and selection                                         */
/* ------------------------------------------------------------------ */

static int always_yes(void) { return 1; }

#ifdef INFER_HAVE_MMX
static int mmx_ok(void) { return bk_cpu_has_mmx(); }
#endif

static const bk_backend backends[] = {
#ifdef INFER_HAVE_AVX2
    /* First: 32 MACs per VPMADDUBSW against MMX's 4 and autovectorised
     * i8's 16. Gated on CPUID *and* OS ymm state, so a machine that
     * cannot run it simply falls through to the next entry.
     *
     * Registered on INFER_HAVE_AVX2 alone, NOT on __AVX2__. This file
     * is compiled at the i486 baseline so one binary runs everywhere,
     * so __AVX2__ is false here even though backend_avx2.c was built
     * with -mavx2 and is linked in. Testing __AVX2__ here would
     * silently drop the kernel from every shipped build -- which is
     * exactly what happened once. */
    { "avx2", "integer kernels with AVX2 VPMADDUBSW inner loop (Haswell+)",
      bk_avx2_available, qmv_avx2 },
#endif
#ifdef INFER_HAVE_VIS
    { "vis", "integer kernels with VIS 1 inner loop (UltraSPARC)",
      bk_vis_available, bk_qmv_vis },
#endif
#ifdef INFER_HAVE_MMX
    { "mmx", "integer kernels with MMX inner loop (Pentium MMX / K6 / Geode+)",
      mmx_ok, qmv_mmx },
#endif
    { "i8",  "integer kernels, portable C (recommended)",
      always_yes, qmv_i8 },
    { "ref", "scalar float reference, maximum portability (486-safe)",
      always_yes, qmv_ref }
};

#define N_BACKENDS ((int) (sizeof(backends) / sizeof(backends[0])))

static const bk_backend *cur = NULL;

/* Is the portable i8 path itself compiled with SIMD?
 *
 * The backend table is a static priority list, which assumes a
 * hand-written kernel always beats portable C. That stops being true
 * the moment the compiler is allowed to vectorise: in a
 * `make linux NATIVE=1` build on a modern x86, autovectorised i8
 * measured 3.27 tok/s against the hand-written MMX kernel's 2.46 --
 * so picking `mmx` first cost 33%.
 *
 * __SSE2__ / __AVX2__ are defined by the compiler only when it is
 * actually allowed to emit those instructions, which is exactly the
 * condition under which i8 gets vectorised. The default i486 build
 * defines neither and is unaffected. (Finding 33.) */
#if defined(__AVX2__) || defined(__SSE2__)
#define I8_IS_VECTORISED 1
#else
#define I8_IS_VECTORISED 0
#endif

static void ensure_selected(void) {
    if (!cur) {
#ifdef INFER_BACKEND
        if (bk_select(INFER_BACKEND) != 0) bk_select("auto");
#else
        bk_select("auto");
#endif
    }
}

int bk_select(const char *name) {
    int i;

    if (!name || !name[0] || strcmp(name, "auto") == 0) {
        /* When the compiler vectorised the portable path, prefer it
         * over the hand-written 64-bit-SIMD kernels. See the comment
         * on I8_IS_VECTORISED.
         *
         * EXCEPT where a hand-written kernel still beats the
         * vectoriser. Finding 33 concluded "prefer i8 once the
         * compiler can vectorise" from a table whose only SIMD
         * alternative was MMX (4 MACs/instr). The AVX2 kernel does 32
         * per VPMADDUBSW, an instruction GCC never emits from this C,
         * so it is ahead of autovectorised i8 rather than behind it.
         * Try it first; if the CPU or OS says no, fall through to the
         * i8 preference exactly as before. */
        for (i = 0; i < N_BACKENDS; i++) {
            if (strcmp(backends[i].name, "avx2") == 0 &&
                backends[i].available()) {
                cur = &backends[i];
                return 0;
            }
        }
        if (I8_IS_VECTORISED) {
            for (i = 0; i < N_BACKENDS; i++) {
                if (strcmp(backends[i].name, "i8") == 0 &&
                    backends[i].available()) {
                    cur = &backends[i];
                    return 0;
                }
            }
        }
        /* first entry that this CPU supports; the table is ordered
         * fastest-first */
        for (i = 0; i < N_BACKENDS; i++) {
            if (backends[i].available()) {
                cur = &backends[i];
                return 0;
            }
        }
        cur = &backends[N_BACKENDS - 1];
        return 0;
    }

    for (i = 0; i < N_BACKENDS; i++) {
        if (strcmp(backends[i].name, name) == 0) {
            if (!backends[i].available()) return -2;
            cur = &backends[i];
            return 0;
        }
    }
    return -1;
}

const char *bk_name(void) { ensure_selected(); return cur->name; }
const char *bk_desc(void) { ensure_selected(); return cur->desc; }

void bk_print_list(FILE *f) {
    int i;
    ensure_selected();

    fprintf(f, "CPU: %s  features:%s%s%s\n", bk_cpu_vendor(),
            bk_cpu_has_mmx()   ? " mmx"   : "",
            bk_cpu_has_3dnow() ? " 3dnow" : "",
            bk_cpu_has_cmov()  ? " cmov"  : "");
    fprintf(f, "\navailable backends:\n");
    for (i = 0; i < N_BACKENDS; i++) {
        fprintf(f, "  %-5s %-3s %s%s\n",
                backends[i].name,
                backends[i].available() ? "ok" : "n/a",
                backends[i].desc,
                &backends[i] == cur ? "   <- selected" : "");
    }
#if !defined(INFER_HAVE_MMX) && !defined(INFER_HAVE_VIS)
    fprintf(f, "\n  (this binary has no SIMD backend compiled in;\n"
               "   it was built with NO_MMX=1 / NO_VIS=1)\n");
#endif
}


/* ------------------------------------------------------------------ */
/* Per-format kernel selection                                         */
/*                                                                     */
/* An override table, empty by default. When a slot is NULL the format */
/* uses whatever --backend chose, so the default behaviour and the     */
/* default cost are both exactly what they were.                       */
/* ------------------------------------------------------------------ */

static const bk_backend *fmt_kernel[BK_F_NFORMATS];

static const struct { const char *name; int slot; int type; } fmt_names[] = {
    { "q4k",  BK_F_Q4_K, GGML_TYPE_Q4_K },
    { "q5k",  BK_F_Q5_K, GGML_TYPE_Q5_K },
    { "q6k",  BK_F_Q6_K, GGML_TYPE_Q6_K },
    { "q8_0", BK_F_Q8_0, GGML_TYPE_Q8_0 }
};
#define N_FMT_NAMES ((int) (sizeof(fmt_names) / sizeof(fmt_names[0])))

static int fmt_slot_of_type(int type) {
    int i;
    for (i = 0; i < N_FMT_NAMES; i++) {
        if (fmt_names[i].type == type) return fmt_names[i].slot;
    }
    return -1;
}

int bk_kernel_set(const char *format, const char *backend) {
    int i, j;
    if (!format || !backend) return -1;
    for (i = 0; i < N_FMT_NAMES; i++) {
        if (strcmp(fmt_names[i].name, format) != 0) continue;
        if (strcmp(backend, "auto") == 0) {
            fmt_kernel[fmt_names[i].slot] = NULL;   /* follow --backend */
            return 0;
        }
        for (j = 0; j < N_BACKENDS; j++) {
            if (strcmp(backends[j].name, backend) != 0) continue;
            if (!backends[j].available()) return -2;
            fmt_kernel[fmt_names[i].slot] = &backends[j];
            return 0;
        }
        return -1;
    }
    return -1;
}

void bk_kernel_list(FILE *f) {
    int i;
    ensure_selected();
    fprintf(f, "per-format kernels (--backend %s is the default):\n",
            cur->name);
    for (i = 0; i < N_FMT_NAMES; i++) {
        const bk_backend *k = fmt_kernel[fmt_names[i].slot];
        fprintf(f, "  %-5s %-5s %s\n", fmt_names[i].name,
                k ? k->name : cur->name,
                k ? "(pinned)" : "(follows --backend)");
    }
}

/* ---- the benchmark ----
 *
 * Synthetic super-blocks, not model weights: this has to run before a
 * model is open, and it must not depend on one being present. The
 * quantised values are arbitrary -- every kernel does the same amount
 * of work regardless of what the bits say -- but the fp16 scales are
 * kept in a sane range so nothing denormalises.                        */

static long bench_rowbytes(int type, long ncols) {
    long nb = ncols / QK_K;
    switch (type) {
        case GGML_TYPE_Q4_K: return nb * 144;
        case GGML_TYPE_Q5_K: return nb * 176;
        case GGML_TYPE_Q6_K: return nb * 210;
        case GGML_TYPE_Q8_0: return (ncols / 32) * 34;
        default:             return 0;
    }
}

static unsigned long bench_rs = 20260805UL;
static unsigned int bench_rnd(void) {
    bench_rs = bench_rs * 1103515245UL + 12345UL;
    return (unsigned int) ((bench_rs >> 16) & 0xFFFFU);
}

void bk_kernel_bench(int verbose) {
    /* One 3584-column row is 14 super-blocks -- the widest row in the
     * 0.8B model, so the cache behaviour resembles the real thing. */
    const long ncols = 3584;
    const int  nrows = 4;
    unsigned char *w;
    float *x, *y;
    long bytes, i;
    int fi, bi, r, reps;

    ensure_selected();

    x = (float *) malloc((size_t) ncols * sizeof(float));
    y = (float *) malloc((size_t) nrows * sizeof(float));
    if (!x || !y) { free(x); free(y); return; }
    for (i = 0; i < ncols; i++) {
        x[i] = ((float) (int) (bench_rnd() & 0x1FF) - 256.0f) / 128.0f;
    }

    if (verbose) {
        inf_log("measuring kernels on this machine "
                "(%ld columns x %d rows)...", ncols, nrows);
    }

    for (fi = 0; fi < N_FMT_NAMES; fi++) {
        int type = fmt_names[fi].type;
        const bk_backend *best = NULL;
        double best_t = 0.0;

        bytes = bench_rowbytes(type, ncols);
        if (bytes <= 0) continue;
        w = (unsigned char *) malloc((size_t) (bytes * nrows));
        if (!w) break;
        for (i = 0; i < bytes * nrows; i++) {
            w[i] = (unsigned char) (bench_rnd() & 0xFF);
        }
        /* fp16 scales: exponent field kept small and positive */
        for (r = 0; r < nrows; r++) {
            long nb, k;
            nb = (type == GGML_TYPE_Q8_0) ? ncols / 32 : ncols / QK_K;
            for (k = 0; k < nb; k++) {
                long o = r * bytes + k *
                    (type == GGML_TYPE_Q4_K ? 144 :
                     type == GGML_TYPE_Q5_K ? 176 :
                     type == GGML_TYPE_Q6_K ? 210 : 34);
                if (type == GGML_TYPE_Q4_K || type == GGML_TYPE_Q5_K) {
                    w[o] = (unsigned char) (bench_rnd() & 0xFF); w[o + 1] = 0x2C;
                    w[o + 2] = (unsigned char) (bench_rnd() & 0xFF); w[o + 3] = 0x2B;
                } else if (type == GGML_TYPE_Q6_K) {
                    w[o + 208] = (unsigned char) (bench_rnd() & 0xFF);
                    w[o + 209] = 0x2C;
                } else {
                    w[o] = (unsigned char) (bench_rnd() & 0xFF); w[o + 1] = 0x2C;
                }
            }
        }

        for (bi = 0; bi < N_BACKENDS; bi++) {
            double t0, t1, dt, bestrun = 0.0;
            int pass;
            if (!backends[bi].available()) continue;
            /* `ref` is the correctness reference, never the fast path;
             * benchmarking it wastes seconds on a slow machine. */
            if (strcmp(backends[bi].name, "ref") == 0) continue;

            backends[bi].matvec(type, w, x, y, ncols, nrows);  /* warm */

            /* Scale the repeat count to the machine: one timed pass
             * first, then enough passes to cover ~40 ms. A 440 MHz
             * Ultra and a modern x86 both end up spending about the
             * same wall time here. */
            t0 = sys_time_sec();
            backends[bi].matvec(type, w, x, y, ncols, nrows);
            t1 = sys_time_sec();
            dt = t1 - t0;
            reps = (dt > 0.0) ? (int) (0.040 / dt) : 20;
            if (reps < 1) reps = 1;
            if (reps > 200) reps = 200;

            for (pass = 0; pass < 3; pass++) {
                int k;
                t0 = sys_time_sec();
                for (k = 0; k < reps; k++) {
                    backends[bi].matvec(type, w, x, y, ncols, nrows);
                }
                t1 = sys_time_sec();
                dt = (t1 - t0) / (double) reps;
                if (pass == 0 || dt < bestrun) bestrun = dt;
            }

            if (verbose) {
                inf_log("  %-5s %-5s %8.3f ms/row-block",
                        fmt_names[fi].name, backends[bi].name,
                        bestrun * 1000.0);
            }
            if (!best || bestrun < best_t) { best = &backends[bi]; best_t = bestrun; }
        }

        if (best) {
            fmt_kernel[fmt_names[fi].slot] = best;
            if (verbose) {
                inf_log("  %-5s -> %s", fmt_names[fi].name, best->name);
            }
        }
        free(w);
    }

    free(x);
    free(y);
}

int bk_kernel_arg(const char *arg) {
    char buf[64];
    char *eq;
    size_t n;

    if (!arg || !arg[0]) return -1;

    if (strcmp(arg, "bench") == 0) { bk_kernel_bench(inf_verbose); return 0; }
    if (strcmp(arg, "list")  == 0) { bk_kernel_list(stdout); return 1; }
    if (strcmp(arg, "auto")  == 0) {
        int i;
        for (i = 0; i < BK_F_NFORMATS; i++) fmt_kernel[i] = NULL;
        return 0;
    }

    n = strlen(arg);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, arg, n + 1);
    eq = strchr(buf, '=');
    if (!eq) return -1;
    *eq = '\0';
    return bk_kernel_set(buf, eq + 1);
}

/* ------------------------------------------------------------------ */
/* Public dispatcher used by the engine                                */
/* ------------------------------------------------------------------ */

/* Row-slice descriptor for the thread pool. The kernels already take
 * a row count, so a slice is just a shifted weight pointer and a
 * shorter run -- no kernel changes were needed to parallelise. */
typedef struct {
    bk_matvec_fn  fn;
    int           type;
    const unsigned char *w;
    const float  *x;
    float        *y;
    long          ncols;
    long          rowbytes;
} mv_job;

static void mv_slice(void *argp, long lo, long hi) {
    const mv_job *j = (const mv_job *) argp;
    if (hi <= lo) return;
    j->fn(j->type, j->w + lo * j->rowbytes, j->x, j->y + lo,
          j->ncols, hi - lo);
}

void q_matvec(int type, const unsigned char *w, const float *x,
              float *y, long ncols, long nrows) {
    PROF_DECL(pt);
    ensure_selected();
#ifdef INFER_SHAPE_LOG
    { static FILE*sf=NULL; if(!sf) sf=fopen("/tmp/shapes.txt","w");
      fprintf(sf,"%d %ld %ld\n",type,ncols,nrows); }
#endif

    PROF_START(pt);
    {
        /* One predictable branch per matrix row-block, against a table
         * that is empty unless someone asked for an override. */
        int slot_ = fmt_slot_of_type(type);
        const bk_backend *k_ = (slot_ >= 0) ? fmt_kernel[slot_] : NULL;
        bk_matvec_fn fn_ = k_ ? k_->matvec : cur->matvec;

        /* Rows are independent, so they split across cores with no
         * locking and no change to the arithmetic: each row is summed
         * by the same code in the same order, just on another core.
         * Bit-identity is asserted by tests/t_thread.c.
         *
         * Only worth the dispatch for matrices with enough rows to
         * amortise it; below that the serial call is faster. The
         * activation quantisation must also happen ONCE, before the
         * split, or every thread would race to fill the same scratch
         * buffer. */
        long rb_ = gg_type_size(type, ncols);
        if (tp_threads() > 1 && nrows >= 64 && rb_ > 0 &&
            type != GGML_TYPE_F32 && type != GGML_TYPE_F16 &&
            (ncols % 32) == 0) {
            mv_job job;
            /* Fill the shared scratch once, then latch it so the
             * workers' own calls become no-ops instead of racing. */
            bk_quantize_x(x, ncols);
            /* The AVX2 k-quant kernels also read the Q8_K plane; build
             * it here too so no worker races to create it. */
            if (type != GGML_TYPE_Q8_0) bk_quantize_k(x, ncols);
            bk_quantize_hold();
            job.fn = fn_; job.type = type; job.w = w; job.x = x;
            job.y = y; job.ncols = ncols; job.rowbytes = rb_;
            tp_parallel_for(nrows, mv_slice, &job);
            bk_quantize_release();
        } else {
            fn_(type, w, x, y, ncols, nrows);
        }
    }

#ifdef INFER_PROFILE
    {
        int slot;
        switch (type) {
            case GGML_TYPE_Q4_K: slot = PF_MATVEC_Q4K; break;
            case GGML_TYPE_Q5_K: slot = PF_MATVEC_Q5K; break;
            case GGML_TYPE_Q6_K: slot = PF_MATVEC_Q6K; break;
            case GGML_TYPE_Q8_0: slot = PF_MATVEC_Q80; break;
            default:             slot = PF_MATVEC_OTHER; break;
        }
        prof_add(slot, prof_now() - pt, (double) ncols * (double) nrows);
    }
#endif
}
