# `src/backend_mmx.c` — MMX kernels

**~850 lines.** The only file in the project using inline assembly, and
the only one that is not strictly conforming ANSI C. Compiled into every
x86 build (`-DINFER_HAVE_MMX`, omit with `make linux NO_MMX=1`), and
selected at run time only when CPUID reports MMX.

## The alignment bug — read this before touching the constants

```c
static const mmx_const c_mask_f_u MMX_ALIGN8 = { 0x000F000F000F000FLL };
```

`movq mem, %mm` **requires an 8-byte-aligned source**. A bare `short[4]`
is only 2-byte aligned per the standard, and GCC gives `.rodata` 4-byte
alignment on 32-bit x86 — so such constants land at addresses ≡ 4 mod 8.
Every one misaligned.

An out-of-order x86 splits a misaligned MMX load transparently, so the
bug is **completely invisible on a modern development machine**. It is a
CPU-specific courtesy, not an architectural guarantee, and it was
reported as a hard failure on real Geode LX / Windows XP hardware.

A `union` with `long long` is **not** sufficient: the i386 SysV ABI
aligns `long long` to 4, not 8. That was tried first and rejected after
checking with `readelf`. An explicit `__attribute__((aligned(8)))` is
required.

`tests/t_align.c` asserts this at run time, so a compiler that ignores
the attribute is caught by `make test` rather than by a crash on the
target.

## The x87 aliasing hazard

MMX registers alias the x87 register stack. Two consequences:

1. **`EMMS` must run** before any float instruction. But it is expensive
   — the Geode's FPU is asynchronous and OLPC measured that "every data
   exchange requires synchronization, which can consume a LOT of
   cycles". So each kernel is **two-phase**: all MMX work for a whole
   matrix row, one `mmx_sync()`, then all the float scale arithmetic.
   That took the count from ~8M `EMMS` per forward pass to ~0.5M.

2. **GCC will reschedule float work into an asm block** unless told not
   to. At `-O2` this produced silent NaNs until `"st"`–`"st(7)"` were
   added to the clobber list. If you write another MMX kernel, do the
   same.

## Kernel shape

Byte-domain masking before widening: one `pand` with `0x0F0F...` extracts
all eight low nibbles at once, versus masking each half separately after
widening.

```
Q4_K  20 -> 16 instructions per 16 weights
Q5_K  52 -> 26   (it was re-loading and re-widening the same data 4x)
```

Q6_K keeps the older shape deliberately: its values carry a −32 bias
which makes them signed, and `punpcklbw` against a zero register
zero-extends. Getting the sign handling wrong there corrupts output
silently, and it is only ~25% of runtime.

## See also

- [../PERFORMANCE-ANALYSIS.md](../PERFORMANCE-ANALYSIS.md) — why the
  kernels are at their floor, and the dead ends
- [../FINDINGS.md](../FINDINGS.md)
