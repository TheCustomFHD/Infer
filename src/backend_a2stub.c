/* backend_a2stub.c -- fallbacks for the AVX2 helpers.
 *
 * backend.c and qwen35.c are compiled at the i486 baseline and call
 * bk_a2_amax / bk_a2_quant256 / bk_a2_dn_recur, which are implemented
 * with intrinsics in backend_avx2.c -- the only object built with
 * -mavx2. Targets that do not link that object (SPARC, s390x, any
 * NO_AVX2=1 build, and several standalone test binaries) still need
 * the symbols to resolve.
 *
 * The condition cannot be __AVX2__: this file is compiled at the i486
 * baseline, where __AVX2__ is false even in a build that DOES link the
 * real helpers. So the build system says so directly -- it defines
 * INFER_A2_LINKED when backend_avx2.o is part of the link, and these
 * fallbacks compile away.
 *
 * Returning 0 means "not done" and the caller runs its scalar loop --
 * the same contract the AVX2 versions use when CPUID or XGETBV says
 * the CPU or OS cannot support them.
 *
 * Strict ANSI C89: no intrinsics, no extensions. This file compiles
 * on a 486, on SPARC and on s390x.
 */

#include "backend.h"

#ifndef INFER_A2_LINKED

int bk_a2_amax(const float *x, long n, float *out) {
    (void) x; (void) n; (void) out;
    return 0;
}

int bk_a2_quant256(const float *x, long n, float id,
                   signed char *q8, int *s16) {
    (void) x; (void) n; (void) id; (void) q8; (void) s16;
    return 0;
}

int bk_a2_dn_recur(float *S, const float *k, const float *q, const float *v,
                   float *kvd, float *o, int hd,
                   float decay, float beta, float scale) {
    (void) S; (void) k; (void) q; (void) v; (void) kvd; (void) o;
    (void) hd; (void) decay; (void) beta; (void) scale;
    return 0;
}

#endif /* !INFER_A2_LINKED */
