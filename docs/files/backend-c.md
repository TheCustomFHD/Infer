# `src/backend.c` — backend selection and the `i8` kernel

**~625 lines.** Owns the public `q_matvec()`, detects CPU features,
quantises activations, and implements the portable integer kernels.

## Why this file exists

Profiling showed **~95% of inference time in one operation**: the
quantised matrix-vector product. Making that swappable — at compile time
or run time — is the whole point.

## The three backends

| name | requires | idea |
|---|---|---|
| `ref` | 486, x87 | dequantise each weight to float, multiply-add |
| `i8` | 486, x87 | quantise activations to int8 once per matrix, then integer MACs |
| `mmx` | MMX | as `i8`, with `PMADDWD` doing 4 MACs per instruction |

Selection is `auto` by default: the table is ordered fastest-first and
the first entry the CPU supports wins. `--backend` overrides;
`-DINFER_BACKEND=\"i8\"` sets a compile-time default.

## Activation quantisation

```c
const bk_qx *bk_quantize_x(const float *x, long n);
```

Splits `x` into 32-element blocks, each with a scale, and stores the
values as `short` (int8 range, widened) so both the scalar and MMX
kernels can consume them without further unpacking.

`s[]` holds the per-block sum, needed because k-quants subtract a
per-block minimum. `s16[]` holds per-16 sums for Q6_K, whose scale
granularity is finer. Precomputing `s16` once per matrix instead of
re-summing eight times per 256-block was a measurable win.

## CPU detection

`CPUID` does not exist on a plain 486. The code checks whether bit 21 of
EFLAGS can be toggled before executing it, and reports no features if
not. The 32-bit PIC path uses the `xchgl %%ebx` dance because `%ebx` is
the PIC register.

## Lessons recorded in the source

**Nibble lookup tables (removed in 1.16.0).** Two 256-byte tables
mapping a packed byte to its two nibbles measured **2.46x** on an
isolated 486-targeted benchmark and **0.97x** on real Geode hardware.
They survived in Q5_K on the theory that its `qh` load made the table
load free. Re-measured in 1.16.0 they were slower than the arithmetic
on every remaining target (sparc64 155 → 147 instructions, i486 2.224 →
1.984 ns/weight), and a table lookup is a gather that no vectoriser can
handle. Both tables are gone; the nibbles are computed. See finding 32.

Two lessons, not one: *a microbenchmark with everything in L1 does not
predict this machine*, and an optimisation kept on a rationale nobody
re-runs will outlive its own justification.

## The i8 floor

The inner loop is 14 instructions producing 2 MACs, two of them `IMUL`.
On the Geode `IMUL` costs ~16 cycles, which is **65% of the backend**.
Measured 21.8 cycles/MAC against a modelled floor of ~22. There is no
slack left — see [../PERFORMANCE-ANALYSIS.md](../PERFORMANCE-ANALYSIS.md)
for the four rejected attempts to remove it.

## See also

- [backend_mmx-c.md](backend_mmx-c.md) · [quant-c.md](quant-c.md)
