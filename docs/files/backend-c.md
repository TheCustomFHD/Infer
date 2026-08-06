# `src/backend.c` — backend selection and the `i8` kernel

**~1350 lines total.** Owns the public `q_matvec()`, detects CPU features,
quantises activations, and implements the portable integer kernels.

## Why this file exists

Profiling showed **~95% of inference time in one operation**: the
quantised matrix-vector product. Making that swappable — at compile time
or run time — is the whole point.

## The four backends

| name | requires | idea |
|---|---|---|
| `ref` | 486, x87 | dequantise each weight to float, multiply-add |
| `i8` | 486, x87 | quantise activations to int8 once per matrix, then integer MACs |
| `mmx` | MMX | as `i8`, with `PMADDWD` doing 4 MACs per instruction |
| `avx2` | AVX2 + OS `ymm` | as `i8`, with `VPMADDUBSW` doing **32** per instruction |

Selection is `auto` by default: the table is ordered fastest-first and
the first entry the CPU supports wins. `--backend` overrides;
`-DINFER_BACKEND=\"i8\"` sets a compile-time default.

Two subtleties in the table, both of which have caused real bugs:

**`avx2` is registered on `INFER_HAVE_AVX2`, never `__AVX2__`.** This
file is compiled at the i486 baseline so one binary runs everywhere, so
`__AVX2__` is false here even when `backend_avx2.o` is in the link.
Testing the codegen macro silently drops the kernel from every shipped
build. (Finding 50.)

**`auto` tries `avx2` before the `I8_IS_VECTORISED` preference.**
Finding 33 concluded "prefer portable `i8` once the compiler can
vectorise" from a table whose only hand-written x86 kernel was MMX at
4 MACs. At 32 the conclusion inverts. The finding-33 rule still applies
below `avx2`, which is why a 64-bit build without AVX2 picks `i8` while
a 32-bit i486-baseline build picks `mmx`.

## Two activation planes

`bk_quantize_x` produces the classic plane: int8 values widened to
int16, **one scale per 32**, plus per-block and per-16 sums. `ref`,
`i8` and `mmx` read it.

`bk_quantize_k` produces a **Q8_K-style** plane: int8, **one scale per
256**, plus per-16 sums. Only the AVX2 k-quant kernels read it, and it
is built lazily — building it on every matvec cost more than it saved,
at ~187 matvecs per forward pass.

Both write shared static buffers, so under threading `q_matvec` fills
them before the split and latches with `bk_quantize_hold()`.

## Threading

`q_matvec` splits output rows across the pool when there are enough of
them. Rows are independent; **dot products are never split**, so the
result is bit-identical at any thread count. See
[`tpool-c.md`](tpool-c.md) for the hazards — the short version is that
no kernel may keep per-call scratch in a `static`.

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

- [backend_mmx-c.md](backend_mmx-c.md)
- [backend_vis-c.md](backend_vis-c.md) — the SPARC kernels · [quant-c.md](quant-c.md)
