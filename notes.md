## 1.24.0 — SPARC: a Solaris madvise trap, and 35% of a token off-kernel

Prompted by a report from real UltraSPARC hardware: 25.88 s/tok
generation, and **"VERY high disk usage"** throughout.

Three defects found, two fixed, one confirmed as a non-fix. Also a
test-coverage gap that had hidden all of it.

### The disk usage: `MADV_SEQUENTIAL` does not mean what it does on Linux

`sys_map_advise_random()` has advised `MADV_SEQUENTIAL` since 1.20.0,
where it correctly replaced `MADV_RANDOM`. On Linux that widens
readahead and nothing more — pages are **not** dropped behind the read
pointer.

Solaris disagrees. Its manual says such pages "will be accessed only
once and **can be freed behind the current access point**". That is
licence to discard.

The access pattern here is sequential *within* one token and then
restarts from the beginning for the next. So the hint is true for a
few milliseconds and false forever after: the pages the next token
needs are exactly the ones Solaris was just told to throw away. On a
machine with less RAM than the 508 MiB model, every generated token
re-reads the model from disk — which is precisely the reported
symptom.

Now `MADV_WILLNEED` only: start pulling the mapping in, with no
licence to evict. Neither hint forces residency, so a small machine
still works; it just pages honestly. x86 is unaffected (14.2 tok/s
before and after). (Finding 56.)

### The compute: SPARC has no integer-to-float register move

Disassembling `vis_dot_q4_K` (sparc64 gcc 14, `-O3`) showed **32
`fmul8x16` against ~250 memory operations**, 99 of 103 stores being
stack spills.

VIS operands live in the **floating-point** register file and
UltraSPARC has no direct move between the integer and FP files, so
every integer-side value fed to a VIS instruction costs a store plus a
load. `-O3` unrolls 4x, multiplies the number of such values live at
once, and the allocator spills. Normalised per `fmul8x16` so the
unroll factor cannot flatter the count:

| flags | insns/mul | spill ops/mul |
|---|---:|---:|
| `-O3` | 19.5 | 4.2 |
| `-O2` | 46.0 | 10.5 |
| `-Os` | 38.8 | 9.2 |
| **`-O2 -funroll-loops`** | **16.0** | **3.7** |

Same ordering on `vis_dot_q5_K` (27.0/7.1 → 21.5/4.7). The Solaris GCC
target now defaults to `-O2 -funroll-loops`, overridable with
`SOLGCCOPT`. Sun Studio is untouched. (Finding 57.)

### Eight function calls per super-block, for one array read

`i8_dot_q6_K` was emitting **nine calls per Q6_K super-block** — eight
of them to `sum16()`, which is a single array subscript. At `-O2` GCC
declined to inline it, so each use became a real call and a SPARC
register-window save/restore, in a kernel that is **35% of a token**
on that machine.

Force-inlining `sum16()` and `rd_f16p()`:

```
i8_dot_q6_K   328 -> 272 insns,  105 -> 54 spills,  9 -> 1 calls
```

Doing the same to `get_scale_min_k4()` was tried and is **worse** —
big enough that inlining it twice per group pushes the share-weighted
total across the three formats from 219 to 239. Reverted. "Small and
hot" is the criterion, not "hot". (Finding 58.)

### What did not work: the VIS Q6_K kernel

Q6_K is 34.9% of a SPARC token and runs on the **portable** path, so
the existing (compiled-out) VIS Q6_K kernel looked like the biggest
single lever. Measured per super-block:

```
i8_dot_q6_K       328 insns, 105 spills
vis_dot_q6_K_al   440 insns, 127 spills
```

VIS is **1.34x more instructions**, matching the user's own
`--kernel bench` (q6k vis 0.711 ms vs i8 0.712 ms). A Q4_K weight is
one nibble and needs one mask; a Q6_K weight is split across two
planes, so reassembling it costs mask+shift+or per weight on the
integer side, and every value then crosses the int→FP gap above. The
format defeats the instruction. Finding 30 was right; it stays
compiled out. (Finding 59.)

### The coverage gap that hid this

`tests/t_kbench.c` — the per-format kernel benchmark — had **no VIS
case at all**. It knew `i8`, `mmx` and `avx2`. So the one platform
whose kernels most needed timing was the one platform that could not
be timed. Added.

### Honest expectation

The instruction-count work is worth roughly **10%** weighted by format
share: 25.88 s/tok to about 23 s/tok. That is short of the 15 s/tok
minimum and well short of the 10 s/tok goal.

**The `madvise` fix is the one that could close the gap, and its size
cannot be measured from here.** qemu has no page-cache pressure and
effectively unlimited RAM, so the failure mode does not reproduce; the
generation rate matches the isolated kernel bench (~57 Mflop/s both
ways), which says the *CPU* sets the rate in the absence of memory
pressure. On the real machine, with "VERY high disk usage" reported,
the disk is doing work the CPU-bound model does not account for. That
needs a measurement on the hardware to size.

### Verification

All SPARC tests pass under qemu with the new flags: `t_viskern` still
reports **exactly `0.000e+00`** on all eleven shapes (VIS remains
bit-identical to `i8`), plus `t_vis`, `t_vissat`, `t_thread` at 1/4,
and `t_endian`. Every source compiles warning-free with
`-O2 -funroll-loops`. x86: all four artifacts build with zero
warnings, all seven host kernel tests pass, output unchanged, and
throughput unchanged at ~14.2 tok/s.


---

### Downloads

| file | notes |
|---|---|
| `infer--windows.exe` | Windows 32-bit -- runs on XP / 486+ / Geode |
| `infer--windows64.exe` | Windows 64-bit |
| `infer--linux` | Linux 32-bit -- runs on a 486 |
| `infer--linux64` | Linux 64-bit |

**One binary per platform.** Every one carries the MMX,
AVX2, i8 and ref kernels and picks the fastest your CPU
supports at startup, after a CPUID probe (plus XGETBV for
AVX2, because Windows XP never saved `ymm` state and an
XP box on modern silicon must fall back, not crash).

There is no ISA ladder to choose from any more: no
`-sse2` or `-avx2` download, and no way to pick wrong.
Check what yours selected with `--backend list`.

Every binary carries **all** the x86 kernels (`mmx`, `i8`,
`ref`) and picks one at run time after a CPUID probe.

All four are built at the **i486 baseline**: only
`backend_avx2.c` is compiled with `-mavx2`, into its own
object, reached through the runtime gate. So the same
file that boots on a Geode uses VPMADDUBSW on a Haswell.

In practice the 32-bit binaries need a **Pentium II or
later**: our own objects contain no CMOV, but glibc and the
mingw CRT do.

Solaris/SPARC is not shipped as a binary -- the toolchain
cannot be redistributed. Build it with `gmake solaris`;
CI cross-compiles and tests it under qemu on every push.

```sh
sha256sum -c SHA256SUMS
```
