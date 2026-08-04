/* t_vis.c -- the assumption the VIS backend is built on.
 *
 * VIS is a graphics instruction set. Its partitioned multiply scales
 * the result for pixel blending:
 *
 *     fmul8x16(u8 a, s16 b) -> (a * b + 0x80) >> 8
 *
 * That >> 8 would destroy our products, which are at most 15 * 127 for
 * a Q4_K weight against an int8 activation. The backend cancels it by
 * pre-shifting the activation left by 8:
 *
 *     fmul8x16(w, x << 8) == w * x        exactly, no rounding
 *
 * Everything in backend_vis.c depends on that identity. This test
 * proves it exhaustively on the machine actually running the code,
 * rather than trusting a manual.
 *
 * Build (GCC):
 *   gcc -mcpu=ultrasparc -mvis -DINFER_HAVE_VIS -o t_vis tests/t_vis.c
 * Build (Sun Studio):
 *   cc -xarch=v9a -DINFER_HAVE_VIS -o t_vis tests/t_vis.c
 *
 * Runs standalone; needs no model.
 */

#include <stdio.h>

#if !defined(INFER_HAVE_VIS)

int main(void) {
    printf("built without INFER_HAVE_VIS; nothing to check\n");
    printf("(rebuild with `make solaris-vis` or `make solaris-gcc-vis`)\n");
    return 0;
}

#else

#if defined(__SUNPRO_C)
#include <vis_proto.h>
/* Sun declares the intrinsics over plain float/double -- a VIS register
 * is an FP register. No vector types, no subscripting; lanes come out
 * through a union. */
typedef float  u8x4;
typedef double s16x4;
typedef union { double d; short s[4]; } lane64;
typedef union { float  f; unsigned int u; } lane32;
#define VMUL8X16(a, b) vis_fmul8x16((a), (b))
#define VADD16(a, b)   vis_fpadd16((a), (b))
#else
typedef unsigned char u8x4  __attribute__((vector_size(4)));
typedef short         s16x4 __attribute__((vector_size(8)));
#define VMUL8X16(a, b) __builtin_vis_fmul8x16((a), (b))
#define VADD16(a, b)   __builtin_vis_fpadd16((a), (b))
#endif

static int fails = 0;

static void ck(const char *what, int ok) {
    printf("%-56s %s\n", what, ok ? "ok" : "** FAIL **");
    if (!ok) fails++;
}

int main(void) {
    int w, x;

    printf("VIS kernel assumptions\n\n");

#if defined(__SUNPRO_C)
    printf("compiler: Sun Studio (<vis_proto.h>)\n\n");
#else
    printf("compiler: GCC/Clang (__builtin_vis_*)\n\n");
#endif

    /* ---- 1. the identity, over every value the kernels can see ----
     *
     * Q4_K weights are 0..15, Q5_K 0..31, Q6_K reaches 63 before the
     * -32 bias is applied, and activations are quantised to
     * [-127, 127]. Check the whole range at once. */
    {
        int bad = 0, n = 0;

        for (w = 0; w < 64; w++) {
            for (x = -127; x <= 127; x++) {
                u8x4  a;
                s16x4 b, r;
                int got, want;

#if defined(__SUNPRO_C)
                {
                    lane32 pa; lane64 pb, pr;
                    /* byte lane 0 is the most significant byte of the
                     * float; short lane 0 the most significant half of
                     * the double. Big-endian SPARC, so index 0 either
                     * way -- and test 4 below proves it. */
                    pa.u = (unsigned int) w << 24;
                    pb.s[0] = (short) (x << 8);
                    pb.s[1] = 0; pb.s[2] = 0; pb.s[3] = 0;
                    a = pa.f; b = pb.d;
                    r = VMUL8X16(a, b);
                    pr.d = r;
                    got = pr.s[0];
                }
#else
                a[0] = (unsigned char) w; a[1] = 0; a[2] = 0; a[3] = 0;
                b[0] = (short) (x << 8);  b[1] = 0; b[2] = 0; b[3] = 0;
                r = VMUL8X16(a, b);
                got = r[0];
#endif
                want = w * x;
                n++;
                if (got != want) {
                    if (bad < 5) {
                        printf("   MISMATCH w=%d x=%d got=%d want=%d\n",
                               w, x, got, want);
                    }
                    bad++;
                }
            }
        }
        printf("checked %d (weight, activation) pairs\n", n);
        ck("fmul8x16(w, x << 8) == w * x, exactly", bad == 0);
    }

    /* ---- 2. x << 8 must not overflow a signed 16-bit lane ---- */
    {
        int ok = 1;
        for (x = -127; x <= 127; x++) {
            long v = (long) x << 8;
            if (v < -32768L || v > 32767L) ok = 0;
        }
        ck("x << 8 stays inside a signed 16-bit lane", ok);
    }

    /* ---- 3. accumulator bounds the kernels rely on ----
     *
     * Products are summed in 16-bit lanes, so each format can only
     * take so many terms before overflow. These are the limits
     * backend_vis.c is written to. */
    ck("Q4_K: 16 terms fit  (16 * 15 * 127 = 30480)",  16 * 15 * 127 <= 32767);
    ck("Q5_K:  8 terms fit  ( 8 * 31 * 127 = 31496)",   8 * 31 * 127 <= 32767);
    ck("Q6_K:  8 terms fit  ( 8 * 32 * 127 = 32512)",   8 * 32 * 127 <= 32767);
    ck("Q4_K: 32 terms would NOT fit (proves the split is needed)",
       32 * 15 * 127 > 32767);

    /* ---- 4. lanes are independent and in order ---- */
    {
        u8x4  a;
        s16x4 b, r;
        int i, ok = 1;

#if defined(__SUNPRO_C)
        {
            lane32 pa; lane64 pb, pr, pp, pq, ps;

            pa.u = 0;
            for (i = 0; i < 4; i++) {
                pa.u |= ((unsigned int) (i + 1)) << (24 - 8 * i);
                pb.s[i] = (short) ((i + 1) << 8);
            }
            r = VMUL8X16(pa.f, pb.d);
            pr.d = r;
            for (i = 0; i < 4; i++) {
                if (pr.s[i] != (short) ((i + 1) * (i + 1))) ok = 0;
            }
            ck("four lanes multiply independently, in order", ok);

            for (i = 0; i < 4; i++) {
                pp.s[i] = (short) (i + 1);
                pq.s[i] = (short) (10 * (i + 1));
            }
            ps.d = VADD16(pp.d, pq.d);
            ok = 1;
            for (i = 0; i < 4; i++) {
                if (ps.s[i] != (short) (11 * (i + 1))) ok = 0;
            }
            ck("fpadd16 adds matching lanes", ok);
            (void) a; (void) b;
        }
#else
        for (i = 0; i < 4; i++) {
            a[i] = (unsigned char) (i + 1);
            b[i] = (short) ((i + 1) << 8);
        }
        r = VMUL8X16(a, b);
        for (i = 0; i < 4; i++) {
            if (r[i] != (short) ((i + 1) * (i + 1))) ok = 0;
        }
        ck("four lanes multiply independently, in order", ok);

        /* and fpadd16 pairs the same lanes */
        {
            s16x4 p, q, s;
            for (i = 0; i < 4; i++) {
                p[i] = (short) (i + 1);
                q[i] = (short) (10 * (i + 1));
            }
            s = VADD16(p, q);
            ok = 1;
            for (i = 0; i < 4; i++) {
                if (s[i] != (short) (11 * (i + 1))) ok = 0;
            }
            ck("fpadd16 adds matching lanes", ok);
        }
#endif
    }

    printf(fails ? "\n%d FAILURE(S)\n" : "\nall VIS assumptions hold\n", fails);
    return fails ? 1 : 0;
}

#endif /* INFER_HAVE_VIS */
