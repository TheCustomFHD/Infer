/* gcc_shim.h -- run the GCC branch of backend_vis.c on x86.
 *
 * The GCC branch calls __builtin_vis_* which exist only on SPARC
 * targets. Compiling with
 *
 *   -D__builtin_vis_fmul8x16=shim_fmul8x16
 *   -D__builtin_vis_fpadd16=shim_fpadd16
 *   -D__builtin_vis_fpadd32=shim_fpadd32
 *   -D__builtin_vis_fmuld8ulx16=shim_fmuld8ulx16
 *   -include tests/vis_shim/gcc_shim.h
 *
 * replaces them with these C functions, which emulate the instruction
 * semantics the same way vis_proto.h does for the Sun Studio path:
 * lane i pairs the i-th byte with the i-th 16-bit halfword, in memory
 * order on both hosts. Type-checking and the register allocation of
 * the pun-union helpers are real; only the instruction emission is
 * emulated. Combined with the vis_proto.h run of the same kernels,
 * both compiler paths are exercised against the i8 reference.
 */
#ifndef INFER_VIS_GCC_SHIM_H
#define INFER_VIS_GCC_SHIM_H

#include <string.h>

/* The vector types are declared in backend_vis.c before any use of the
 * builtins, so at this point (included after the file's includes, via
 * -include) we must only declare functions; the types come from the
 * macros' expansion context. To keep the signatures simple we use
 * void* and cast inside, relying on vector types being passed in
 * registers with the same ABI as their element aggregates. */

#define SHIM_VIS_TYPES                                                  \
    typedef unsigned char shim_u8x4  __attribute__((vector_size(4)));   \
    typedef short         shim_s16x4 __attribute__((vector_size(8)));   \
    typedef short         shim_s16x2 __attribute__((vector_size(4)));   \
    typedef int           shim_s32x2 __attribute__((vector_size(8)))

SHIM_VIS_TYPES;

static shim_s16x4 shim_fmul8x16(shim_u8x4 a, shim_s16x4 b) {
    unsigned char ab[4]; short bs[4], rs[4]; shim_s16x4 r; int i;
    memcpy(ab, &a, 4); memcpy(bs, &b, 8);
    for (i = 0; i < 4; i++)
        rs[i] = (short) (((int) ab[i] * (int) bs[i] + 0x80) >> 8);
    memcpy(&r, rs, 8);
    return r;
}

static shim_s16x4 shim_fpadd16(shim_s16x4 x, shim_s16x4 y) {
    short a[4], b[4], c[4]; shim_s16x4 r; int i;
    memcpy(a, &x, 8); memcpy(b, &y, 8);
    for (i = 0; i < 4; i++) c[i] = (short) (a[i] + b[i]);
    memcpy(&r, c, 8);
    return r;
}

static shim_s32x2 shim_fpadd32(shim_s32x2 x, shim_s32x2 y) {
    int a[2], b[2], c[2]; shim_s32x2 r; int i;
    memcpy(a, &x, 8); memcpy(b, &y, 8);
    for (i = 0; i < 2; i++) c[i] = a[i] + b[i];
    memcpy(&r, c, 8);
    return r;
}

static shim_s32x2 shim_fmuld8ulx16(shim_u8x4 a, shim_s16x2 b) {
    unsigned char ab[4]; short bs[2]; int c[2]; shim_s32x2 r; int i;
    memcpy(ab, &a, 4); memcpy(bs, &b, 4);
    for (i = 0; i < 2; i++) c[i] = (int) ab[i + 2] * (int) bs[i];
    memcpy(&r, c, 8);
    return r;
}

#endif /* INFER_VIS_GCC_SHIM_H */
