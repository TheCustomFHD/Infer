/* t_align.c -- guard against the MMX constant misalignment bug.
 *
 * `movq mem, %mm` requires an 8-byte-aligned source operand. The MMX
 * constants in backend_mmx.c are declared with an alignment attribute
 * to guarantee that. This test asserts it actually happened.
 *
 * The bug this guards against is invisible on an out-of-order x86 --
 * such CPUs split a misaligned MMX load transparently -- but shows up as
 * a hard failure on real Geode LX / Windows XP hardware. So it cannot be
 * caught by "it runs on my machine"; it has to be asserted.
 *
 * Builds to a no-op success when the MMX backend is not compiled in.
 */

#include <stdio.h>

#ifdef INFER_HAVE_MMX

/* Defined in backend_mmx.c. */
extern const void *mmx_const_addrs[5];

static const char *names[5] = {
    "c_mask_f", "c_mask_1", "c_mask_3", "c_8", "c_32"
};

int main(void) {
    int i, bad = 0;

    printf("MMX constant alignment (movq requires %% 8 == 0):\n");
    for (i = 0; i < 5; i++) {
        unsigned long a = (unsigned long) mmx_const_addrs[i];
        int off = (int) (a & 7UL);
        printf("  %-10s %p  %% 8 = %d  %s\n",
               names[i], mmx_const_addrs[i], off,
               off ? "MISALIGNED  <-- would fault on strict hardware" : "ok");
        if (off) bad++;
    }

    if (bad) {
        printf("\nFAIL: %d of 5 MMX constants are not 8-byte aligned.\n", bad);
        printf("The aligned(8) attribute in backend_mmx.c was ignored.\n");
        return 1;
    }
    printf("\nall MMX constants correctly aligned\n");
    return 0;
}

#else

int main(void) {
    printf("MMX backend not compiled in (-DINFER_HAVE_MMX absent);"
           " alignment test skipped.\n");
    return 0;
}

#endif
