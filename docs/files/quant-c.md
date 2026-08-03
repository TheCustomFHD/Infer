# `src/quant.c` — dequantisation and the reference kernel

**~490 lines.** Decodes GGML block-quantisation formats and provides
`qmv_ref`, the scalar float matrix-vector product that every other
backend is checked against.

## Formats

| type | block | bytes | layout |
|---|---|---|---|
| Q4_0 | 32 | 18 | fp16 d, 16 B nibbles; `q = d*(n-8)` |
| Q8_0 | 32 | 34 | fp16 d, 32 B int8 |
| Q4_K | 256 | 144 | fp16 d, fp16 dmin, 12 B packed 6-bit scales/mins, 128 B nibbles |
| Q5_K | 256 | 176 | as Q4_K plus 32 B of high bits |
| Q6_K | 256 | 210 | 128 B low nibbles, 64 B high bits, 16 B int8 scales, fp16 d |

Plus F32 and F16 passthrough. Layouts follow `ggml-common.h` exactly —
verified value-by-value against a reference decoder.

## `get_scale_min_k4`

Q4_K and Q5_K pack eight 6-bit scales and eight 6-bit minimums into
12 bytes. This mirrors ggml's own unpacking. Getting it wrong produces
output that looks *almost* right, which is the worst failure mode, so it
is worth reading carefully rather than reimplementing.

## Fused dot products

`qmv_ref` does not dequantise a whole row into a buffer and then
multiply. It decodes one block and consumes it immediately:

```c
static float dot_q4_K(const unsigned char *b, const float *x, long ncols);
```

Keeping the decoded values in registers rather than round-tripping
through memory matters on a machine with an 8 KB L1.

## fp16 conversion

`q_fp16_to_fp32` is pure integer bit manipulation, including the
subnormal path. No `_Float16`, no intrinsics, no lookup table.

## Role

Since 1.1.0 this file provides the **reference**, not the default.
`backend.c` owns the public `q_matvec` and dispatches to `ref`, `i8` or
`mmx`. When a fast kernel is suspected, `--backend ref` is ground truth.

## `q_fp16_to_fp32` and the width trap

The scale of every k-quant block is an fp16, so this is one of the
hottest small functions in the program. It builds the fp32 bit pattern
in an integer and copies it out.

That integer is an `unsigned long` — 4 bytes on i486, 8 on LP64. The
original `memcpy(&out, &bits, 4)` therefore copied the *high* half on a
64-bit big-endian host: every fp16 became 0.0, every quantisation scale
in the model became zero, and generation produced garbage without a
single error. It now narrows to a fixed 32-bit object first.

Worth knowing before touching it: extracting the four bytes by hand is
equally correct but stops the compiler folding the store into one
instruction, and measured 4% end-to-end. See
[../FINDINGS.md](../FINDINGS.md) findings 18 and 19.

## See also

- [backend-c.md](backend-c.md) · [backend_mmx-c.md](backend_mmx-c.md)
- [../PERFORMANCE-ANALYSIS.md](../PERFORMANCE-ANALYSIS.md)
