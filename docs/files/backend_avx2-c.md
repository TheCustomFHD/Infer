# `src/backend_avx2.c` — AVX2 kernels

**~960 lines.** Intrinsics (`<immintrin.h>`), so like `backend_mmx.c`
it is not strictly conforming ANSI C. It is **the only object in the
project compiled with `-mavx2`**, and that fact drives everything
below.

## Why it is compiled separately

Every other translation unit is built at the **i486 baseline**
(`-march=i486 -mno-sse -mno-sse2`) so that one binary runs on a 486 and
still accelerates on a modern CPU. Compiling this file in the same
command would apply `-mavx2` project-wide and the binary would fault
before reaching `main()`.

So the Makefile does two stages:

```make
# stage 1 -- this file only
$(CC) $(A2FLAGS) -c -o $(BUILD)/backend_avx2.o $(SRCDIR)/backend_avx2.c
# stage 2 -- everything else at the baseline, linking that object
$(CC) $(X86FLAGS) -o $(BIN) $(CORE) ... $(A2_OBJ)
```

Verified in CI: every artifact must contain >100 `ymm` **and** >100 MMX
instructions **and** an `xgetbv`. If the two stages ever collapse into
one command, either the baseline is contaminated or AVX2 vanished, and
one of those assertions fails.

## The run-time gate checks the OS, not just the CPU

```c
int bk_avx2_available(void) { return bk_cpu_has_avx2(); }
```

`bk_cpu_has_avx2()` requires **four** things to agree: `CPUID.1:ECX`
bits 27 (OSXSAVE) and 28 (AVX), `XCR0` bits 1 and 2 via `XGETBV`, and
`CPUID.7:EBX` bit 5 (AVX2).

The `XGETBV` half is not optional. AVX adds architectural state — the
upper half of `ymm` — so the OS must promise to save it across a
context switch. **Windows XP never did**, so an XP machine on Haswell
silicon reports AVX2 in CPUID and raises `#UD` on the first VEX
instruction. With only the CPUID half, `auto` would select a backend
that crashes instead of falling back to `mmx`. This project ships an
XP target, so it is a live case, not a hypothetical.

## The kernel shape

The instruction that matters is **`VPMADDUBSW`**: 32 multiply-
accumulates per instruction against `PMADDWD`'s 16 and MMX's 4. It
needs one *unsigned byte* operand, which is exactly what an unpacked
k-quant weight is (Q4_K 0..15, Q5_K 0..31, Q6_K 0..63) — and the
activations are already int8. GCC never emits it from the portable C
because that code widens to `int` before multiplying.

Per 256-weight superblock the kernel:

1. reads activations from the **Q8_K-style plane** (`xq->k8`), which
   has one scale per 256 values rather than per 32;
2. applies the weight's 6-bit scale **in the integer domain**, folded
   into the `VPMADDWD` that had to widen int16→int32 anyway;
3. keeps **one int32 accumulator** for the whole superblock;
4. converts to float exactly once.

Step 1 is what makes 2–4 possible. An earlier version reduced and
converted every 32 weights: eight horizontal sums and ~32 scalar float
ops per superblock, which cost more than the multiplies.

Per-superblock accumulation is also what keeps it **safe** —
accumulating a 248320-column row in int32 would overflow, while per
superblock the worst case (Q6_K) is 66× inside the limit.
`tests/t_avx2sat.c` checks the saturation budget exhaustively rather
than by argument.

Kernels process **two rows at a time**: the activation plane is shared,
so a one-row kernel reloads it `nrows` times. Four rows was measured
and is *worse* (register pressure).

## Not bit-identical, and that is deliberate

The activation is quantised per 256 rather than per 32, the sum is
reassociated across lanes, and the scale is applied before the float
conversion. All legitimate reorderings; none exact.

`tests/t_ident.c` therefore does **not** cover these kernels.
`tests/t_avx2acc.c` replaces it, bounding relative L2 error against the
**float reference** and requiring AVX2 to be no worse than portable
`i8`. Measured 2.2e-03 – 8.0e-03 against i8's 2.8e-03 – 5.7e-03.

`-ffp-contract=off` is part of `AVX2ARCH` and CI greps for it:
`-march=haswell` otherwise fuses `a*b+c` into `vfmadd`, which keeps the
product at full width and made the AVX2 build diverge from every other
build at around token 8. It costs 2.8% and buys reproducibility.

## The `bk_a2_*` helpers

Three functions here exist only because `backend.c` and `qwen35.c` are
baseline-compiled and may not contain intrinsics:

| function | replaces |
|---|---|
| `bk_a2_amax` | the abs-max scan in `bk_quantize_k` |
| `bk_a2_quant256` | the float→int8 quantise loop |
| `bk_a2_dn_recur` | the Gated DeltaNet recurrence in `qwen35.c` |

Each returns **1 if it did the work, 0 if AVX2 is unavailable**, so the
caller keeps a plain scalar fallback and needs no `#ifdef`.

Targets that do not link this object — SPARC, s390x, `NO_AVX2=1`, and
several standalone tests — get the symbols from
**`src/backend_a2stub.c`**, which is always compiled and guarded by
`INFER_A2_LINKED`. That macro is set by the *Makefile*, not derived
from `__AVX2__`: the stub is compiled at the baseline where `__AVX2__`
is false even in a build that does link the real helpers. Getting this
wrong yields either duplicate or missing symbols.

## See also

- [`backend-c.md`](backend-c.md) — the dispatcher and the backend table
- [`../FINDINGS.md`](../FINDINGS.md) — findings 47, 48, 49, 50
