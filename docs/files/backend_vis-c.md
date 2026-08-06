# `src/backend_vis.c` — VIS 1 kernels for UltraSPARC

**~890 lines.** The SPARC counterpart to `backend_mmx.c`: hand-written
partitioned-integer matvec kernels, compiled only under
`-DINFER_HAVE_VIS` and selected at run time only on a CPU that has
VIS. Registers as the `vis` backend.

Like `backend_mmx.c` this is the *only* place the project leaves
strict C89 on SPARC — it uses GCC/Sun Studio vector extensions. It is
also the only backend built by two quite different compilers, which is
most of the maintenance cost.

## The central trick: making `fmul8x16` exact

VIS is a **graphics** instruction set. Its partitioned multiply is
defined for pixel blending, so it scales the result:

```
fmul8x16(u8 a, s16 b)  ->  s16   (a * b + 0x80) >> 8
```

Taken literally that is useless for inference: a Q4_K nibble times an
int8 activation is at most `15 * 127 = 1905`, and `>> 8` throws almost
all of it away.

But the shift **cancels**. Pre-shift the activation left by 8 before
it reaches the kernel and:

```
fmul8x16(w, x << 8) = (w * (x << 8) + 0x80) >> 8 = w * x
```

exactly, with no rounding error, provided `x << 8` still fits a signed
16-bit lane. Activations are quantised to `[-127, 127]`, so `x << 8`
spans `[-32512, 32512]` and fits.

Verified exhaustively against the real instruction semantics — 16,320
combinations of `w` in 0..63 and `x` in −127..127, zero mismatches —
and `tests/t_vis.c` re-runs that check on the target machine. The
pay-off is that a graphics instruction becomes an exact **four-lane**
integer multiply, the same width as MMX's `pmaddwd`, instead of
falling back to `fmuld8ulx16` at two lanes.

## Accumulator width is a budget, not an assumption

Products stay in 16-bit lanes, so partial sums must not pass ±32767.
The kernels accumulate a **bounded** number of terms and then widen:

| format | max weight | terms | worst lane | headroom |
|---|---|---|---|---|
| Q4_K | 15 | 16 | 30480 | 7.0% |
| Q5_K | 31 | 8 | 31496 | 3.9% |
| Q6_K | \|32\| | 8 | 32512 | 0.8% |
| Q8_0 | \|127\| | 2 | 32258 | 1.6% |

Q6_K sits 0.8% under the limit, which is close enough that it is
**tested rather than asserted**: `tests/t_vissat.c` drives every
weight to maximum and every activation to the ±127 clamp and requires
bit-identity with `i8`.

Widening uses `fmuld8ulx16` against a constant 1 byte — an exact
16→32-bit expansion of two lanes — because VIS has no unpack
instruction that would do it directly.

## Bit-identical to `i8`, deliberately

Unlike the AVX2 kernels, `vis` is **exactly** the same arithmetic as
the portable integer path: same terms, same order, same integer
domain. `tests/t_viskern.c` compares against `qmv_i8` and prints the
worst relative error, which should read `0.000e+00` on every shape —
anything else means the exactness argument has broken somewhere, even
though the test's own pass threshold is a looser `1e-4`. Treat a
small-but-nonzero number as a failure to investigate, not a pass.

The reference for that test is `qmv_i8`, and that matters. Against
`qmv_ref` the VIS kernels show 1–12% relative error, which is int8
activation quantisation — visible in `i8` too — not a kernel fault.
Comparing against the wrong reference sends you debugging correct
code.

## Q6_K is compiled out by default

`INFER_VIS_Q6K` gates the Q6_K kernel, and it is **off**. Not because
it is wrong — it passes every test — but because it measured no faster
than `i8` on the real machine. Q6_K was never instruction-bound there,
so a 3.1× instruction reduction was invisible end to end (finding 30).

There is no `make` target for it; the per-variant target ladder was
removed in 1.21.0. Pass the define by hand:

```sh
gmake solaris SUNOPT='-xO5 -xunroll=16 -DINFER_VIS_Q6K'
```

The default should be the path that measured faster, and re-measuring
is the only way to change that.

## Two compilers, one file

Sun Studio and GCC spell VIS completely differently, so most helpers
appear twice behind `#if defined(__SUNPRO_C)`: `vis_hsum16`,
`vis_zero16`, `vis_widen2`, `vis_hsum32`, `vis_pack_u8x4`. Studio uses
`vis_*` intrinsics from `<vis_proto.h>`; GCC uses
`__attribute__((vector_size))` types and builtins.

Most of the risk in maintaining that is type errors, and those can be
caught **without a Sun machine**. `tests/vis_shim/vis_proto.h`
reimplements Sun's prototypes in plain C, so compiling with
`-D__SUNPRO_C -Itests/vis_shim` takes the Studio branch under GCC:

```sh
make vis-shim-test
```

It proves types and arithmetic only. GCC inlines the shim's C instead
of emitting VIS instructions, so **instruction counts taken this way
are meaningless** — only Studio can be judged on Studio's codegen.

## Entry points

| symbol | role |
|---|---|
| `bk_qmv_vis()` | the matvec entry registered in the backend table |
| `bk_vis_available()` | run-time capability check |
| `bk_vis_free_scratch()` | releases the pre-shifted activation scratch |

`xs_prepare()` builds the `x << 8` plane the whole trick depends on,
once per matvec rather than once per row.

## See also

- [backend-c.md](backend-c.md) — the table this registers into
- [backend_mmx-c.md](backend_mmx-c.md) — the same job on x86
- [tests.md](tests.md) — `t_vis`, `t_viskern`, `t_vissat`
- [../FINDINGS.md](../FINDINGS.md) — finding 23 (making `fmul8x16`
  exact), 25 (two compilers, and a test that never ran), 29 (the lane
  budgets), 30 (why the Q6_K work measured as nothing)
