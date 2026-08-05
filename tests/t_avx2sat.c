/* t_avx2sat.c -- VPMADDUBSW cannot saturate on any format we feed it.
 *
 * VPMADDUBSW multiplies two adjacent (u8, s8) pairs and adds them into
 * one int16 lane, WITH SATURATION. If a lane ever saturates the result
 * is silently wrong -- not an exception, just a clamped number. That is
 * precisely the failure mode the VIS lane-budget work walked into
 * (findings 29 and 35), where the reasoning was right in the abstract
 * and wrong about the actual budget.
 *
 * So this is checked exhaustively rather than argued. The worst case
 * per output lane is
 *
 *     2 * wmax * |x|max      with |x|max = 127
 *
 * because the activations are quantised to [-127, 127] by
 * bk_quantize_x. This test walks every (w, x) pair for every format's
 * weight range and confirms the true product stays inside int16.
 *
 * Also verifies the second stage: VPMADDWD against a vector of ones
 * widens the int16 pairs to int32, and that sum must not overflow
 * either over a full 32-weight group.
 *
 * Runs standalone, needs no model, and is meaningful on any host --
 * it is arithmetic about the format, not about the instruction, so it
 * is worth running even where AVX2 is unavailable.
 */
#include <stdio.h>
#include <limits.h>

static int fail = 0;

static void check_format(const char *name, int wmax, int per_lane_pairs,
                         int lanes_per_group) {
    int w, x;
    long worst_pair = 0;
    long worst_group;

    /* Stage 1: VPMADDUBSW, two (u8 x s8) products summed into int16. */
    for (w = 0; w <= wmax; w++) {
        for (x = -127; x <= 127; x++) {
            long p = (long) w * (long) x;
            long two = 2 * p;                 /* both lanes worst case */
            if (two >  worst_pair) worst_pair =  two;
            if (-two > worst_pair) worst_pair = -two;
        }
    }
    if (worst_pair > 32767L) {
        printf("  %-6s ** SATURATES ** worst int16 lane %ld > 32767\n",
               name, worst_pair);
        fail = 1;
    } else {
        printf("  %-6s wmax %2d  worst int16 lane %6ld  (limit 32767, "
               "%.1f%% used)\n",
               name, wmax, worst_pair, 100.0 * (double) worst_pair / 32767.0);
    }

    /* Stage 2: VPMADDWD sums pairs of those into int32, then we add
     * lanes_per_group of them together. */
    worst_group = worst_pair * (long) per_lane_pairs * (long) lanes_per_group;
    if (worst_group > 2147483647L || worst_group < -2147483647L) {
        printf("  %-6s ** int32 ACCUMULATOR OVERFLOWS ** %ld\n",
               name, worst_group);
        fail = 1;
    }
}

int main(void) {
    printf("VPMADDUBSW saturation budget\n");
    printf("(activations are clamped to [-127,127] by bk_quantize_x)\n\n");

    /* Weight ranges AFTER unpacking, before any scale is applied:
     *   Q4_K  4-bit nibble                        0..15
     *   Q5_K  4-bit nibble + high bit worth 16    0..31
     *   Q6_K  4-bit low | 2-bit high, biased      0..63
     * Q8_0 is signed and does not use VPMADDUBSW at all.
     *
     * per_lane_pairs: int16 lanes combined by one VPMADDWD  = 2
     * lanes_per_group: int32 lanes summed per accumulator   = 8 */
    check_format("Q4_K", 15, 2, 8);
    check_format("Q5_K", 31, 2, 8);
    check_format("Q6_K", 63, 2, 8);

    printf("\nQ8_0 uses signed weights -> VPMADDWD on widened int16,\n"
           "which cannot saturate by construction (checked by t_ident).\n");

    if (fail) {
        printf("\nFAIL: a format can saturate. The AVX2 kernel is unsafe.\n");
        return 1;
    }
    printf("\nall formats fit with margin -- VPMADDUBSW is safe here\n");
    return 0;
}
