/* t_scales.c -- the word-parallel k-quant scale decode is exact.
 *
 * a2_scales8() unpacks six 6-bit scales and six 6-bit mins from twelve
 * packed bytes. The obvious implementation walks the eight indices with
 * a branch and several shifts each; the one actually used treats the
 * twelve bytes as four 32-bit words and does the whole thing with three
 * masks and no branch.
 *
 * That rewrite was worth 5x on what profiling showed to be the single
 * largest term in the AVX2 Q4_K kernel -- larger than the vector
 * arithmetic it feeds (finding 53) -- so it is worth proving the two
 * forms agree rather than assuming it.
 *
 * The packed layout is only 12 bytes, but 2^96 is not enumerable, so
 * this checks a large pseudo-random sample plus the boundary patterns
 * that bit-packing bugs actually hide in (all-zero, all-ones, single
 * bits, and every value in the two-bit carry positions).
 *
 * Runs standalone; needs no model. Pure integer arithmetic, so it is
 * meaningful on every target, not just x86.
 */
#include <stdio.h>
#include <string.h>

/* The reference: one index at a time, exactly as the format is
 * documented and as the scalar kernels have always done it. */
static void ref_scale_min(int j, const unsigned char *q,
                          unsigned char *d, unsigned char *m) {
    if (j < 4) {
        *d = (unsigned char) (q[j] & 63);
        *m = (unsigned char) (q[j + 4] & 63);
    } else {
        *d = (unsigned char) ((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        *m = (unsigned char) ((q[j + 4] >>  4) | ((q[j    ] >> 6) << 4));
    }
}

/* The implementation under test, copied from backend_avx2.c. Kept as a
 * copy on purpose: this file must build without AVX2 so the check runs
 * everywhere, including SPARC and a 486. */
static void word_scales8(const unsigned char *q, unsigned char *sc,
                         unsigned char *mn) {
    const unsigned int k1 = 0x3f3f3f3fu;
    const unsigned int k2 = 0x0f0f0f0fu;
    const unsigned int k3 = 0x03030303u;
    unsigned int u[4], aux;

    memcpy(u, q, 12);
    u[3] = ((u[2] >> 4) & k2) | (((u[1] >> 6) & k3) << 4);
    aux  = u[1] & k1;
    u[1] = (u[2] & k2) | (((u[0] >> 6) & k3) << 4);
    u[2] = aux;
    u[0] &= k1;

    memcpy(sc,     &u[0], 4);
    memcpy(sc + 4, &u[1], 4);
    memcpy(mn,     &u[2], 4);
    memcpy(mn + 4, &u[3], 4);
}

static int check(const unsigned char *q, const char *what, long n) {
    unsigned char sc[8], mn[8];
    int j, bad = 0;

    word_scales8(q, sc, mn);
    for (j = 0; j < 8; j++) {
        unsigned char d, m;
        ref_scale_min(j, q, &d, &m);
        if (d != sc[j] || m != mn[j]) {
            if (bad < 3)
                printf("  %s #%ld idx %d: scale %u vs %u, min %u vs %u\n",
                       what, n, j, (unsigned) d, (unsigned) sc[j],
                       (unsigned) m, (unsigned) mn[j]);
            bad = 1;
        }
    }
    return bad;
}

int main(void) {
    unsigned char q[12];
    unsigned long rs = 20260806UL;
    long i;
    int b, bad = 0;

    printf("k-quant scale decode: word-parallel vs per-index\n\n");

    /* all zero / all ones */
    memset(q, 0x00, 12); bad += check(q, "zeros", 0);
    memset(q, 0xFF, 12); bad += check(q, "ones", 0);

    /* every single bit set on its own -- catches a wrong mask or shift */
    for (i = 0; i < 96; i++) {
        memset(q, 0, 12);
        q[i / 8] = (unsigned char) (1u << (i % 8));
        bad += check(q, "single bit", i);
    }

    /* saturate the two-bit carry fields, which is where the packing is
     * least obvious */
    for (b = 0; b < 4; b++) {
        memset(q, 0, 12);
        q[0] = (unsigned char) (b << 6);
        q[1] = (unsigned char) (b << 6);
        q[2] = (unsigned char) (b << 6);
        q[3] = (unsigned char) (b << 6);
        bad += check(q, "carry bits", b);
    }

    /* and a large random sample */
    for (i = 0; i < 2000000L; i++) {
        int k;
        for (k = 0; k < 12; k++) {
            rs = rs * 1103515245UL + 12345UL;
            q[k] = (unsigned char) ((rs >> 16) & 0xFF);
        }
        if (check(q, "random", i)) { bad = 1; break; }
    }

    if (bad) {
        printf("\nDECODE MISMATCH -- the fast path is not equivalent\n");
        return 1;
    }
    printf("  zeros, ones, 96 single bits, carry patterns, 2000000 random\n");
    printf("\nword-parallel decode is exact\n");
    return 0;
}
