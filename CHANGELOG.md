# Changelog

## 1.18.0 — Windows 64-bit, and an SSE2/AVX2 ladder

Windows users download binaries; Linux users generally build their own.
So Windows now gets the full matrix and Linux keeps two.

| binary | baseline | runs on |
|---|---|---|
| `infer-X.Y.Z-windows.exe` | i486, x87 | anything, incl. Geode and XP |
| `infer-X.Y.Z-windows-sse2.exe` | Pentium 4 | P4, Athlon 64 and later |
| `infer-X.Y.Z-windows64.exe` | x86-64 | any 64-bit x86 |
| `infer-X.Y.Z-windows64-avx2.exe` | Haswell | 2013 and later |
| `infer-X.Y.Z-linux` | i486, x87 | anything |
| `infer-X.Y.Z-linux64` | x86-64 | any 64-bit x86 |

Measured on one Xeon, 20 tokens, greedy, best of 3:

```
32-bit i486 baseline   1.61 tok/s
32-bit SSE2            2.01 tok/s   1.25x
64-bit                 2.27 tok/s   1.41x
64-bit AVX2            2.90 tok/s   1.80x
```

The gain is not a new kernel — it is finding 33 doing its job. Once the
compiler may emit SSE2, it vectorises the portable `i8` path, `auto`
notices and stops choosing the hand-written MMX kernel. Verified: the
32-bit baseline selects `mmx`, all three higher tiers select `i8`.

**`ISA=` is not `NO_SSE2=`.** The `NO_*` flags choose which kernels are
compiled in and gated on CPUID at run time. `ISA=` moves the compiler's
baseline for *all* code, so those builds have no fallback — an AVX2
binary faults on a CPU without AVX2. That is why they are separate
downloads rather than one binary.

**No 32-bit AVX2 build**, deliberately: every AVX2-capable x86 part is
64-bit, so it would have no hardware to run on.

### The AVX2 build was giving different answers

Greedy decoding with a fixed seed diverged at roughly token 8:

```
...Paris. It serves as the nation's political, cultural, and economic center.
...Paris. It serves as the nation's largest city and the seat of its government,
```

Not a kernel bug — `tests/t_ident` still reports every integer kernel
bit-identical. `-march=haswell` lets GCC contract `a*b+c` into a single
`vfmadd` (27 of them in `quant.c` alone), which keeps the product at
full width instead of rounding to float first. More accurate, and
different from every other build.

`-ffp-contract=off` costs **2.83 → 2.75 tok/s (2.8%)** and makes the
AVX2 build byte-identical to the SSE2 and 64-bit builds. Taken: someone
who switches binaries for speed and gets different text will reasonably
conclude the fast one is broken. CI now asserts the flag is present and
that zero `vfmadd` reach the shipped exe.

The 32-bit baseline still differs, and that one is unavoidable — x87
keeps 80-bit intermediates. Proven rather than assumed: rebuilding
32-bit with `-mfpmath=sse` reproduces the 64-bit output exactly.

### The i486 promise is untouched

Byte-for-byte unchanged, re-verified: 0 CMOV, 0 xmm/ymm, 206 MMX
instructions, selects `mmx`, runs under an emulated Pentium II. Our
objects are 486-clean under both `gcc -m32` and mingw.

To be precise about what that means, since it came up: the 32-bit
binaries still need a **Pentium II**, and always did. Our code emits no
CMOV, but the runtime does — on Linux `ld.so` faults first
(`cmovnel` in `_dl_aux_init`, confirmed by tracing `-cpu 486`), and on
Windows the mingw CRT has 36 CMOV, though all of them sit in
`strtod`/`pow`/`ldexp` helpers rather than on the startup path.

### Other

- All shipped binaries now carry the per-stage timers. Overhead was not
  measurable (2.443–2.477 profiled vs 2.430–2.466 plain, overlapping)
  and logging is opt-in via `--log-stages`, so the separate `-profile`
  artifacts are gone. `PROFSUF=-profile` restores the old naming.
- `make all` builds six binaries; `make help` lists the matrix.
- Two new CI steps: every Windows tier is asserted to really be that
  ISA (baseline 0 xmm / sse2 >100 xmm 0 ymm / avx2 >50 ymm, all with
  3 XP-era DLLs), and the float-contraction guard above.

## 1.17.5 — CI: the probe now compiles what the build compiles

1.17.4 worked: the toolchains installed, and steps 1–9 passed for the
first time in five runs. Run #34 then failed at step 10, `make all`,
exit code 2 — and the step *before* it had just declared
`x86_32: yes`.

Both faults are mine, and they are the same fault twice.

**The probe was easier than the real build.** It compiled
`int main(void){return 0;}`. That needs no kernel headers, so it links
happily in an environment where `asm/errno.h` is missing — which is
precisely the environment finding 41 describes. `src/net_posix.c`
includes `<errno.h>`, needs those headers, and dies. Reproduced
exactly by hiding the i386 headers:

```
$ cc -m32 hello.c            # links fine -> probe says "x86_32: yes"
$ cc -m32 uses_errno_h.c
/usr/include/linux/errno.h:1:10: fatal error: asm/errno.h: No such file
```

A capability check that is weaker than the thing it gates is not a
check. The probe now compiles the headers `net_posix.c` actually uses
(`errno.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`), and mingw
gets the `winsock2.h`/`ws2tcpip.h` equivalent. Verified both ways: it
passes on a healthy runner, and on a runner with the i386 headers
removed it fails at the probe with the compiler's own error and a
sentence naming `linux-libc-dev:i386`.

**Finding 41 came back, because Ubuntu is not Debian.** There,
`linux-libc-dev` is `Architecture: all`; on Ubuntu it is arch-qualified
(`amd64`, `i386`). `libc6-dev-sparc64-cross` depends on
`linux-libc-dev-sparc64-cross`, and satisfying the cross libcs can
replace the shared `linux-libc-dev` and drop the i386 headers with it.
apt is behaving correctly — nothing in that transaction said i386 was
needed. 1.17.4 installed the optional cross toolchains *after* the
required set, so it re-opened the exact hole 1.17.3 closed.

Now `linux-libc-dev:i386` is named explicitly and re-asserted **after**
the cross toolchains, so the 32-bit headers are a stated property of
the final state rather than a survivor of dependency resolution.

No source change. `infer` is byte-identical to 1.17.4 apart from the
version string.

## 1.17.4 — CI: a failed toolchain install can no longer report success

Run #32 failed at "confirm the win32 (not posix) mingw variant" with
exit code 127 — `i686-w64-mingw32-gcc: not found`. mingw was not the
problem. **No compiler was installed at all**, including plain
`cc -m32`, and the run had already been green for four steps.

Two mistakes combined, and 1.17.3 contained both.

**`apt-get install` is atomic.** Given ten package names, if one cannot
be resolved apt exits 100 and installs *none* of the other nine.
1.17.3 put every toolchain — required and optional — on a single line
and appended `|| echo "::warning::some cross-toolchains unavailable"`.
That message was written for the case where a *cross* compiler is
missing. In practice one unavailable optional package took out
`gcc-multilib`, `libc6-dev-i386` and mingw too, and the `||` reported
it as a warning.

**The probe was advisory when it should have been decisive.** The next
step correctly detected that all four targets were unusable and emitted
four warnings — then let the run continue. The first step to actually
invoke a compiler died on `command not found`, three steps and one
misleading error message away from the cause.

Fixed structurally, not by adding another package name:

- **Required and optional toolchains are now separate transactions.**
  The required set (`build-essential`, `gcc-multilib`,
  `libc6-dev-i386`, `gcc-mingw-w64-i686-win32`,
  `binutils-mingw-w64-i686`, `qemu-user-static`) has **no `||`** — if
  the shipped artifacts cannot be built, the run says so at the step
  that caused it.
- **Optional cross targets install one package at a time**, so an
  unavailable sparc64 compiler costs sparc64 and nothing else. Each
  gets its own warning naming the package and the target it disables.
- **The probe now fails the run** if `x86_32` or `win32` cannot compile
  a hello world, and prints the compiler's own stderr plus a sentence
  saying which package did not install. `sparc` and `s390x` stay
  advisory — they gate their own steps.

Both groups still install before anything is compiled, which is what
finding 41 requires: pulling a cross-toolchain mid-run once dragged in
a `linux-libc-dev` that removed the i386 headers `gcc-multilib` needs,
breaking a target that had built an hour earlier.

No source, Makefile or kernel change. `infer` itself is byte-identical
to 1.17.3 apart from the version string.

## 1.17.3 — CI fixes: one apt transaction, probed targets, surviving artifacts

Everything in 1.17.0, plus the two CI faults that run #24 exposed.

**IF YOU UPDATE BY COPYING THIS TREE OVER YOUR CHECKOUT, DELETE THIS
FILE BY HAND:**

```sh
git rm .github/workflows/release.yml
```

Copying files over a checkout cannot remove anything. That workflow is
the second publisher described below, and it will keep publishing
unverified releases until it is gone.

### CI: one apt transaction, then probe what exists

The x86 build failed with `asm/errno.h: No such file` **after** having
built successfully in the same job. Installing the SPARC cross-compiler
mid-run pulled a `linux-libc-dev` change that removed the i386 headers
`gcc-multilib` needs.

Every toolchain is now installed in **one** apt transaction, with
`libc6-dev-i386` named explicitly, and a new `probe` step compiles and
runs a trivial program per target to record what is actually usable.
SPARC and s390x steps are gated on that probe rather than on an
assumption. See finding 41.

### CI: steps must not delete the artifacts later steps need

`--kernel selection` failed with exit 127 — command not found. The
flag-matrix and SPARC steps end with `make clean`, which removes the
x86 binaries that every later step refers to by name. Reproduced
locally: build, `make clean`, then `./infer-X.Y.Z-linux` gives "No such
file or directory".

Every destructive step now ends with `make all`. Verified by replaying
the whole step order: 6 artifacts built, 6 restored after the flag
matrix, 6 after the SPARC steps, all present at the upload.

The `windows exe imports` and `--think` steps had the same latent
dependency and survived only because an intervening step happened to
rebuild.

### The emulated SPARC run no longer gates the build

Run #26 failed on `t_vis` under qemu — the VIS exactness test, which
passes here on qemu 10.0.11 and fails on the runner's qemu 8.2.2. I
could not reproduce it (Debian trixie has no qemu 8 to install) and I
am not going to guess a third time: QEMU's `fmul8x16` rounding differs
between those versions in form (`if ((tmp & 0xff) > 0x7f) tmp += 0x100`
versus `+ 0x80`), but both produce identical results for every input
this kernel uses, so that is not the explanation.

The step now prints the compiler and emulator versions and is marked
`continue-on-error`, so it reports without blocking a release. The
SPARC **cross-build** still gates — that is real compilation and its
failures are real. What is emulated is advisory until it can be run on
hardware.

This is the same judgement as finding 30: an emulator models semantics,
not the machine, and this project has been misled by emulated results
before. A check that cannot be reproduced locally should not be able to
stop a release.

### The SPARC step failed on a missing package, not a bug

`gcc-sparc64-linux-gnu` only *Recommends* `libc6-dev-sparc64-cross`, so
`libc.a` may be absent. `-static` then produces a dynamic binary and
qemu dies at load time:

```
qemu-sparc64-static: Could not open '/lib64/ld-linux.so.2'
```

The cross libc is now installed explicitly, and the step **proves the
toolchain can build and run a static sparc64 binary** before enabling
the SPARC checks — a capability test rather than an installation test.
The qemu loop also reports which test failed; `for t in ...; do qemu
$t; done` had been returning only the last iteration's status.

### A release could be published for a failed build

`build.yml` gates this correctly and always has:

```yaml
release:
  needs: build
  if: github.ref_type == 'tag'
```

Run #24 confirms it — build failed, release ran 0s, nothing published.

The culprit was `.github/workflows/release.yml`: a second workflow on
the same `v*` tag trigger with **no `needs:` and no `if:`**, left over
from before the two were merged. It rebuilt from scratch and published
regardless. That is how v1.15.1 got a release from a red build, and it
means the binaries on releases up to that tag are not the ones CI
verified.

Deleted in 1.16.0 — but only in this tree. See the note at the top.

## 1.17.0 — per-format kernel selection, and a benchmark that runs on your machine

`--backend` picks one kernel for every weight format. That is a good
default and a poor assumption: the formats have different shapes, and a
kernel that wins on one can lose on another. Q6_K on UltraSPARC is the
worked example — its VIS kernel cuts instructions 3.1x and measured
**0.8%** (finding 30), so it ships off, based on measurements from one
machine.

Now the machine can decide for itself:

```sh
infer --kernel bench -v        # measure here, pin the winners
infer --kernel list            # show the current assignment
infer --kernel q6k=i8          # pin one format by hand
infer --kernel auto            # back to following --backend
```

`bench` needs **no model** — it builds synthetic super-blocks, so it
works before you have downloaded anything, and it never touches real
weights. It scales its repeat count to the host, targeting ~40 ms per
measurement, so a 440 MHz Ultra 10 and a modern x86 spend about the
same wall time. Three timed passes per kernel, best taken.

```
$ infer --kernel bench -v
measuring kernels on this machine (3584 columns x 4 rows)...
  q4k   mmx      0.029 ms/row-block
  q4k   i8       0.032 ms/row-block
  q4k   -> mmx
  ...
```

### Why this is safe

Mixing kernels per format is only legitimate if the kernels agree
numerically. They do, on every format, bit for bit — `tests/t_ident.c`
compares with `memcmp`, not a tolerance, and CI runs it. If that ever
fails, `--kernel` becomes a correctness hazard rather than a
performance knob, and the test says so.

### Cost

The dispatcher gained one table lookup per matrix row-block. Measured
against a build with the table compiled out, interleaved A/B, six
samples each:

```
  table=2.210  bypass=2.384
  table=2.407  bypass=2.393
  table=2.373  bypass=2.410
  table=2.450  bypass=2.415
  table=2.382  bypass=2.248
  table=2.423  bypass=2.350
```

Fully overlapping — the table wins three of six. The lookup happens
once per row-block, which is ~113,000 weights of work, so it is far
below the noise floor. The table is empty unless you ask for something,
in which case the code path is exactly what it was.

### It is a measurement tool, not an optimiser

On the 64-bit build the benchmark picked a mixed assignment no single
`--backend` could express (`q5k=mmx`, everything else `i8`) — and that
mix measured **3.316 tok/s against 3.334** for plain `auto`. Slightly
worse.

The synthetic benchmark runs one format at a time with everything hot.
Real inference interleaves formats across 24 layers and streams ~344 MB
per forward pass. A kernel that wins in isolation can lose in that
traffic — the same effect that made the isolated i8 nibble LUT measure
2.46x and deliver 0.97x (finding 28).

So `bench` answers "which kernel is faster here, for this format, in
isolation" — the question that previously needed a rebuild. Confirm
with `--log-perf` on the real model before trusting the combination.
This is also why it is opt-in and not run at startup: automatic
selection would have made this host slower. See finding 38.

### CI fix: the SPARC step, and a stray publisher

**SPARC tests failed under qemu.** `gcc-sparc64-linux-gnu` only
*Recommends* `libc6-dev-sparc64-cross`, so `libc.a` may be absent;
`-static` then yields a dynamic binary and qemu dies with `Could not
open '/lib64/ld-linux.so.2'`. The cross libc is now installed
explicitly, and the step proves it can build **and run** a static
sparc64 binary before enabling the SPARC checks. The test loop also
reports which test failed instead of returning only the last one's
status.

**A release could be published for a failed build.** `build.yml` gates
this correctly (`needs: build`), and the v1.17.0 run proves it — build
failed, release ran 0s, nothing published. The culprit was
`.github/workflows/release.yml`, a second workflow on the same `v*`
tag trigger with no `needs:` at all, which rebuilt and published
independently. It was deleted in 1.16.0; older tags show the symptom.
See finding 40.

### CI fix: v1.16.1 could not build

The v1.16.1 tag failed in 15 seconds at the toolchain install, exit
code 100, before compiling anything. `gcc-sparc64-linux-gnu` — added
for the new SPARC checks — is in Ubuntu's **universe**, which the
runners do not enable, and `apt-get` fails the whole command when one
package is missing.

`universe` is now enabled up front, and the SPARC toolchain install is
non-fatal: if it is unavailable the three SPARC steps skip with a
warning instead of failing the release. `qemu-user-static` and the
mingw packages are also in universe and were working only by accident
of the runner image.

The v1.16.1 tag itself was sound — rebuilt locally it produces all six
artifacts and passes every assertion. See finding 39.

### Also

- `--log-stages` now names the right build command (`make <target>
  PROFILE=1`) instead of three targets removed in 1.16.0
- new test `t_ident`, wired into `make i8-split-test` and CI

## 1.16.1 — everything since 1.14.2

The last release on GitHub is **1.14.2**. This rolls up five versions of
work: a Q6_K VIS kernel (later switched off on the evidence), the first
real-hardware verdict from an UltraSPARC IIi, a build system collapsed
to three targets, and several corrections to this project's own
measurements.

Per-version detail is in the sections below; this is the summary.

### Performance

| change | effect | where |
|---|---|---|
| Q5_K redundant fold removed | **2.03x** on Q5_K | SPARC, real hardware |
| i8 Q4_K split into two reductions | 1.25x x86-64, 1.11x i486 | all targets |
| Q5_K nibble lookup table removed | 1.12x, and unblocks vectorising | all targets |
| Q5_K loop shape chosen by build flags | **1.88x** under SSE2 | SSE2/AVX2 builds |
| `auto` prefers vectorised i8 | **1.33x** | `NATIVE=1` / `BITS=64` |

On the SPARC the Q5_K fix is the headline: the long-standing anomaly
where Q5_K cost 2.55x a Q4_K call, despite being only 25% wider, was a
fold guarding an overflow that could not happen. Three baseline runs
agreed to three significant figures before and after.

End to end on a modern x86 host, same model and seed:

```
make linux                      2.45 tok/s    (i486 floor, picks mmx)
make linux BITS=64              3.11          (picks vectorised i8)
make linux BITS=64 NATIVE=1     4.23          1.73x
```

### Build system

Sixteen make targets became **three** — `linux`, `windows`, `solaris` —
plus four test targets. Everything else is a flag: `PROFILE=1`,
`NATIVE=1`, `BITS=64`, `TOOLCHAIN=gcc`, `FAST=1`, and
`NO_MMX=1` / `NO_SSE2=1` / `NO_AVX2=1` / `NO_VIS=1`.

`make all` now builds all six shipping artifacts (32- and 64-bit Linux,
32-bit Windows, each with and without timers). It previously built two,
and CI uploaded a `windows-profile.exe` that nothing produced.

### Negative results kept

Three optimisations were written, measured, and **not** adopted or
turned off by default. They are in the tree, tested, and documented:

- **Q6_K VIS kernel** — correct, bit-identical, 3.1x fewer
  instructions, and **0.8% faster** on an UltraSPARC IIi. Off by
  default; `-DINFER_VIS_Q6K` enables it. Q6_K was never
  instruction-bound: per weight it is the *cheapest* format on every
  platform measured, and only looked expensive because its calls carry
  6x more weights than Q4_K's.
- **64-bit VIS loads** — 1395 → 1477 instructions. Rejected.
- **Copy-based alignment for Q6_K** — 5% fewer instructions but 192
  bytes of copy traffic per super-block through a 16 KB direct-mapped
  cache. Rejected.

### Corrections to our own measurements

- **i8 vs mmx was never 0.44x.** That compared a 1.0.0 log with a 1.6.0
  log. Same version they are within 2–7%: at 1.0.0, Geode 0.026 vs
  0.028, i7 1.236 vs 1.265. The gap is the 1.3.0–1.5.0 MMX rewrite, not
  the instruction set. (Finding 37.)
- **The i486 baseline is real; the binary still needs a Pentium II.**
  Our objects contain zero CMOV and zero SSE/AVX, asserted per-file in
  CI. glibc's `_dl_aux_init` does not — a static `printf("hi")` at
  `-march=i486` dies the same way. (Finding 34.)
- **Instruction counts are not a cost model**, on any of these targets.
  The i486 Q4_K kernel gained 8 instructions and got 1.11x *faster*.
  (Finding 36.)
- **qemu does not enforce CPU feature masks** in user mode: `-cpu
  pentium2` will happily run SSE2. It can prove the 486 case, which is
  a real decode failure, and nothing about SSE2 gating.

### CI

Now cross-compiles and tests **SPARC** on every push (sparc64 GCC +
qemu: `t_vis`, `t_viskern`, `t_vissat`, `t_endian`, with and without
VIS), exercises every build flag, asserts the shipped 32-bit binaries
contain **no unconditional SSE/AVX**, checks each artifact selects the
intended backend, and runs the binary under an emulated Pentium II.

Release notes are extracted from the newest `CHANGELOG.md` section,
with a guard that fails the release if that section is not for the tag
being built.

### Also

- prompt-prefix cache kept after review: ~100 lines, two call sites,
  saves minutes per request on a 440 MHz machine
- `.github/workflows/release.yml` deleted — it was a second, older
  releaser racing the one in `build.yml` on the same tags
- new tests, none needing a model: `t_vissat`, `t_i8split`,
  `t_q5split`, `t_viskern`, and a VIS shim that runs both compiler
  branches on any host
- findings 27–37 added to `docs/FINDINGS.md`

---

## 1.16.0 — three targets, everything else a flag

### Build system

Sixteen make targets collapsed to **three**:

```sh
make linux      make windows      make solaris
```

Plus four test targets (`test`, `solaris-test`, `vis-shim-test`,
`i8-split-test`) and the usual utilities. Everything else is a flag:

| flag | effect |
|---|---|
| `PROFILE=1` | per-stage timers; binary named `...-profile` |
| `NATIVE=1` | `-march=native`; runs only on the build machine |
| `BITS=64` | 64-bit Linux, named `...-linux64` (default 32-bit) |
| `TOOLCHAIN=gcc` | Solaris via GCC instead of Sun Studio |
| `FAST=1` | Solaris non-IEEE float (`-fast` / `-Ofast`), opt-in |
| `NO_MMX=1` `NO_SSE2=1` `NO_AVX2=1` `NO_VIS=1` | leave a kernel out |

`solaris-vis-test` and `solaris-gcc-vis-test` were the same three
programs built by different compilers; they are now
`solaris-test [TOOLCHAIN=gcc]`, which also builds `t_endian` and
`t_backend`. `solaris-fast` became `FAST=1`.

Implemented with indirect variable expansion (`$(VAR_$(FLAG))`) rather
than `ifeq`, because this Makefile also has to work with Solaris
`/usr/ccs/bin/make`, which has no conditionals.

### Auto-selection was giving away 33% in NATIVE builds

The backend table is a static priority list that assumes hand-written
SIMD always beats portable C. Under `-march=native` that is false:

```
make linux NATIVE=1
  --backend mmx    2.456 tok/s    <- what auto picked
  --backend i8     3.274 tok/s    <- 1.33x faster
```

`auto` now prefers `i8` when `__SSE2__` or `__AVX2__` is defined, i.e.
exactly when the compiler was allowed to vectorise it. The i486 build
defines neither and is untouched — `i8_dot_q4_K` disassembles
byte-identically, and the object is 64 bytes smaller. See finding 33.

### Two bugs found while adding NATIVE

**`-march=native` appended after the i486 baseline did nothing useful.**
`-mno-sse -mno-sse2` are sticky and survive a later `-march`, so the
result was a native-*tuned* binary emitting no SIMD at all. `NATIVE=1`
now **replaces** the baseline: default build 0 SIMD instructions,
`NATIVE=1` build 716.

**Solaris was always building native.** `SUNTARGET` was hardcoded to
`native`, so every Sun Studio binary was silently machine-specific. The
default is now `-xtarget=generic`. GCC on SPARC needs `-mcpu=native`,
not `-march=native`, and cross-compilers reject it — override with
`SOLCPU_1=-mcpu=ultrasparc3`.

`FAST=1` is toolchain-aware for the same reason: Studio spells it
`-fast`, GCC `-Ofast`, and passing the wrong one is a hard build error.

### Q5_K picks its loop shape from the build flags

Splitting the Q5_K nibble loops was measured and rejected in 1.15.3 --
scalar, it costs loads (i486 165 -> 170 instructions). Under SSE2 the
verdict inverts completely:

```
matvec Q5_K   3.489 s -> 1.853 s      1.88x
end to end    2.756   -> 3.308 tok/s  1.20x
```

Without it, an SSE2 build was nearly pointless: Q4_K 1.87x and Q6_K
1.28x faster, but Q5_K **0.56x** -- backwards -- eating the win and
leaving 1.07x. It is 20% of the weights and had grown to 48% of matvec
time.

The shape is now chosen by `#if defined(__SSE2__) || defined(__AVX2__)`.
The i486 object is byte-identical to before. `tests/t_q5split.c` proves
both shapes agree over 200,000 random sub-rows on four targets. See
finding 35.

### The i486 baseline is real; the binary needs a Pentium II

Our objects are clean — zero CMOV, zero SSE/AVX in every translation
unit, now asserted per-file in CI. The *linked* binary is not: glibc's
`_dl_aux_init` uses CMOV, so `qemu-i386 -cpu 486` kills it before
`main()`. A static `printf("hi")` at `-march=i486` fails identically.

```
486 / pentium    Illegal instruction (glibc)
pentium2 and up  mmx <- selected
```

The Geode is unaffected: 486-class core *with* MMX, behaves as
`pentium2`+. A genuine 486 needs static musl or uClibc-ng — documented,
not tested here. Same shape as the mingw CRT finding. See finding 34.

### Corrected: i8 vs mmx was never 0.44x

`docs/PERFORMANCE-ANALYSIS.md` compared `perf-i7-9th-i8.txt` (**1.0.0**)
with `perf-i7-9th-mmx-1.6.0.txt` (**1.6.0**) and reported the difference
as a backend gap. Same-version numbers:

```
1.0.0 Geode   i8 0.026   mmx 0.028   0.93x
1.0.0 i7      i8 1.236   mmx 1.265   0.98x
1.6.0 Geode   i8 0.030   mmx 0.063   0.48x
```

At 1.0.0 the two were within 2-7%. The gap is the 1.3.0-1.5.0 MMX
rewrite, not the instruction set. `ref` has always been ~3x behind i8
end to end and 6.4x behind on Q6_K. See finding 37.

### `make all` now builds everything shippable

It built two artifacts and CI uploaded a `windows-profile.exe` that
nothing produced. Now six:

```
infer-X.Y.Z-linux                 32-bit, i486 floor, picks mmx
infer-X.Y.Z-linux-profile         ...with timers
infer-X.Y.Z-linux64               64-bit, picks the SSE2-vectorised i8
infer-X.Y.Z-linux64-profile       ...with timers
infer-X.Y.Z-windows.exe           32-bit, i486 floor, picks mmx
infer-X.Y.Z-windows-profile.exe   ...with timers
```

`NATIVE=1` is deliberately not a release artifact: it runs only on the
machine that built it. CI uploads all six and asserts each picks the
right backend — 32-bit `mmx`, 64-bit `i8` with >100 SIMD instructions.

### CI

`build.yml` now cross-compiles and tests **SPARC** on every push
(sparc64 GCC + qemu: `t_vis`, `t_viskern`, `t_vissat`, `t_endian`,
with and without VIS), exercises every build flag, and asserts the
shipped x86 binaries contain **no unconditional SSE/AVX** — the check
that makes "one generic binary" a fact rather than an intention.

Release notes are now extracted from the newest `CHANGELOG.md` section
instead of a fixed blurb, with a guard that fails the release if that
section is not for the tag being built.

### Q5_K no longer uses a lookup table

`i8_dot_q5_K` read its nibbles through two 256-byte tables. They were
slower than the arithmetic they replaced, on every target still in use:

```
i8_dot_q5_K, static instruction count
  sparc64  155 -> 147   (35 -> 30 loads)
  i486     168 -> 165

Q5_K, i486-targeted build, ns/weight, best of 7
  table       2.224
  arithmetic  1.984      1.12x
```

Bit-identical output on x86-64, i486 and big-endian SPARC.

They were also a hard blocker for any autovectoriser, because a table
lookup is a gather. In an experimental `-march=haswell` build Q4_K and
Q6_K sped up 2.2x and 1.7x while Q5_K stayed scalar and grew to 56% of
matvec time, cancelling the win — 3.18 tok/s with the tables against
4.58 without. That build is not part of the project, but the blocker it
exposed is real for the SPARC builds too.

The comment justifying the tables claimed "~2.4x on the Q4_K inner
loop". Q4_K has never used them. See finding 32.

### PROFILE=1 instead of nine duplicate targets

The per-stage timers are still a compile-time choice, but no longer a
separate rule per target:

```sh
make linux PROFILE=1          # == make linux-profile
make solaris-vis PROFILE=1    # == make solaris-vis-profile
```

`PROFILE=1` appends `-DINFER_PROFILE` and renames the binary to
`...-profile`, so the two builds cannot collide. Every `*-profile`
target still exists as a one-line alias, so nothing that used them
breaks, and the nine duplicated compile lines are gone.

The timers stay compile-time rather than a runtime flag deliberately:
`PROF_STOP` sits inside the per-layer path, so a runtime test would put
a load-and-branch in the hot loop of every build — including the 440
MHz Ultra and the 500 MHz Geode, where it is measurable.

## 1.15.3 — i8 Q4_K, one reduction per nibble stream

`i8_dot_q4_K` interleaved both nibble streams in a single loop. Split
into two clean reductions it is measurably faster on every x86 target,
and bit-identical:

```
                     before    after   speedup
x86-64                0.747    0.598     1.25x
i486/geode (32-bit)   1.844    1.707     1.08x     (median of 10)
```

ns per weight, 3584-column rows, 8 rows, 60 repeats. The i486 figure is
the 486-baseline **build** running on a modern host, so it predicts
direction, not magnitude — the real Geode has a different cache and
issue width.

This is the portable `i8` backend, so it also lands on SPARC, Solaris
and every other target with no MMX. Verified bit-identical there too.

### Why this is interesting

**"Split the loops" was already measured and rejected for MMX at
0.90x.** Same transformation, same format, opposite sign. In hand-written
MMX the two streams share loaded registers and splitting forces a
re-load and re-mask; in portable C, separating them lets the compiler
unroll and pipeline each reduction freely. A transformation is good or
bad against a particular register allocator, not in the abstract — so
the rejected list is worth re-testing per backend, not treated as
settled. See finding 31.

### Measured and rejected

- **Byte-domain nibble masking** ported from the MMX kernel: 0.78 →
  1.03 ns/weight. Hand-assembling a 32-bit word in C costs more than
  the byte loads it replaces.
- **Splitting Q5_K the same way**: 1.43 → 1.51 ns/weight. Q5_K reads two
  planes (`qs` and `qh`), so splitting doubles the loads. Only Q4_K is
  split.

### New

- `tests/t_i8split.c` — 200,000 random sub-rows per architecture,
  comparing the integer accumulators directly. Needs no model.

A note for anyone repeating this: comparing the two kernels through
their **float** results reports spurious differences on `-mfpmath=387`
builds. That is x87 80-bit excess precision spilling at different
points across translation units, not an arithmetic change. The integer
accumulators are identical on x86-64, i486 and big-endian SPARC.

### Also confirmed

The per-weight cost trend from finding 30 holds on x86, not just SPARC:
Q6_K is the cheapest format per weight on Geode, i7 and UltraSPARC
alike, and Q4_K carries 40% of the weights on all of them.

## 1.15.2 — first real-hardware verdict, and Q6_K switched off

An UltraSPARC IIi finally ran this. One change is a large win, one is
worthless, and the profile that motivated the second was misread.

### Q5_K: confirmed, ~2x faster

The 1.15.1 fold removal is real and reproducible. Within-run ratios,
which cancel any machine-state difference between sessions:

```
Q5_K / Q4_K cost per call
  1.14.2 run #1   3.19x
  1.14.2 run #2   3.19x      three runs, identical to 3 s.f.
  1.14.2 run #3   3.19x
  1.15.1          1.57x      2.03x faster
```

Per weight, Q5_K went from ~1152 ns to ~617 ns. The long-standing
"Q5_K costs 2.55x Q4_K despite being 25% wider" anomaly was a redundant
fold, and it is gone.

### Q6_K: kernel disabled by default

The Q6_K VIS kernel cuts instructions 3.1x and measured **0.8%** on
hardware:

```
Q6_K / Q4_K cost per weight, same run
  1.14.2, portable i8 path   0.862
  1.15.1, VIS kernel         0.855
```

`make solaris-vis` now routes Q6_K to `qmv_i8`, which measured faster.
The kernel is kept and still tested — build `make solaris-vis-q6k` (or
`-DINFER_VIS_Q6K`) to enable it. When disabled it is compiled out
entirely, along with its four helpers, so the default binary carries no
dead code.

### Why the profile was misleading

Grouped by `us/call`, Q6_K looked like the most expensive format. Call
sizes are not comparable:

```
Q4_K   113,338 elements/call
Q5_K   155,345 elements/call
Q6_K   678,142 elements/call     <- 6x a Q4_K call
```

Per weight, across every log going back to 1.11.0, **Q6_K has always
been the cheapest format on this machine** — ~427 ns/weight against 495
for Q4_K and 1152 for Q5_K, even with no VIS kernel. It is 27% of
matvec time because it is 40% of the weights. See finding 30.

### Note on the 1.15.1 log

That run came from a different session and is inflated throughout:
`rms norms`, which no version has touched, is 4.3x slower in it. Absolute
times are not comparable across the two logs; the ratios above are.
Anyone repeating this should run the versions back to back in one
session.

## 1.15.1 — lane budgets, re-derived

Three kernel-level wins, all pure deletion, all bit-identical to `i8`.

```
                1.15.0   1.15.1   change   share of SPARC runtime
Q4_K               261      253    -3.1%   36.7%
Q5_K               391      343   -12.3%   34.4%
Q6_K              1504     1338   -11.0%   27.2%
```

Weighted across the format mix: **8.5% fewer matvec instructions**, on
top of 1.15.0. Static instruction counts per super-block, sparc64
cross-GCC `-O2`, fully unrolled.

### Q5_K was folding four times more often than needed

The kernel widened its 16-bit lanes to 32-bit on every inner iteration,
citing an 8-term budget. The budget is right; the loop only ever puts
**one** term per lane per iteration, four in total — `4 * 31 * 127 =
15748` against a 32767 limit. The fold was guarding an overflow that
could not happen, and cost 12 extra `hsum16` per sub-block.

This looks like the explanation for a long-standing oddity in the SPARC
profile: Q5_K costing **2.55× the µs/call of Q4_K** despite being only
25% wider. Width never explained that gap. Four times the fold traffic
does.

### Q6_K holds four terms instead of two

Same class of error, opposite direction: it widened every iteration
when a lane can carry all four terms. Worst case `4 * 63 * 127 = 32004`
against 32767 — it fits, with 2.3% to spare.

That margin is too thin to leave as arithmetic on paper, so
`tests/t_vissat.c` now drives every lane to its worst case (all weights
at maximum, all activations at the ±127 clamp) and requires bit-identity
with `i8`. Q5_K and Q6_K both pass.

The aligned Q6_K path is now **1236** instructions, below the 1423 of
the copy-based approach it was measured against in 1.15.0 — the
copy-free version now wins on instruction count as well as cache
behaviour.

### Chain folding

Q4_K and Q5_K run two independent accumulator chains to hide the
3-cycle VIS latency, then called `hsum16` twice. One `fpadd16` before a
single `hsum16` is cheaper and keeps the chains independent through the
loop, since the merge happens after it.

### New

- `tests/t_vissat.c` — worst-case lane saturation. Needs no model.

### Still unmeasured

Instruction counts, not cycles. No UltraSPARC has run this code. The
8.5% assumes cycles track instructions, which on an in-order machine
with a 16 KB direct-mapped D-cache they may not.

## 1.15.0 — Q6_K VIS kernel

Q6_K was 27.2% of measured SPARC runtime and had no VIS kernel; it fell
through to the portable integer path. It now has one.

```
Q6_K, per 256-weight super-block, sparc64 cross-GCC -O2, unrolled:
  scalar i8                     ~17    ops/MAC
  VIS, 8-aligned super-block      5.45 ops/MAC   (1395 instrs)
  VIS, 2-aligned super-block      6.30 ops/MAC   (1614 instrs)
  averaged over the alternation   5.88 ops/MAC
```

About 2.9x fewer instructions than `i8` on the format that is a quarter
of all matvec work, including `token_embd` and the LM head.

### How it works

A Q6_K weight is six bits — four from the `ql` plane, two from `qh` —
and then biased by −32. `fmul8x16`'s first operand is unsigned, so the
biased value cannot be fed to it. The kernel multiplies the *unbiased*
weight and folds the bias through the block sums:

```
sum((w - 32) * x) = sum(w * x) - 32 * sum(x)
```

`bk_quantize_x` already computes `sum(x)` per 16 activations, which is
Q6_K's scale granularity. This is the same arithmetic `i8_dot_q6_K`
performs, in the same order, so the results are **bit-identical**.

Unbiased `w` is 0..63 and `|x| <= 127`, so one product reaches 8001 and
two fit a 16-bit lane. Products are paired before widening, halving the
widen traffic.

### Alignment without copying

Q6_K's 210-byte stride makes super-block alignment alternate 0, 2, 0,
2, ... within a row, so it cannot be decided per row — only per
super-block. Aligned blocks take `lduw`; the rest assemble from two
`lduh`. No copy, no extra cache traffic.

### Fixed

- **Byte order in the weight loader.** Assembling a lane word
  little-endian reverses the weights on SPARC while the activations
  stay in order — a wrong dot product, silently, and invisible on x86.
  Only formats with odd strides (Q6_K, Q8_0, Q4_0) are affected;
  Q4_K/Q5_K use a native aligned load and were never at risk. See
  finding 27.
- `make vis-shim-test` no longer claims `-DINFER_BIG_ENDIAN=1` while
  running on a little-endian host, and prints what it does and does not
  check.

### New

- `make vis-shim-test` — runs **both** compiler branches on any host.
  `tests/vis_shim/gcc_shim.h` emulates `__builtin_vis_*` the way
  `vis_proto.h` covers the Sun Studio path. Types, register allocation
  and arithmetic are real; lane order is *not* checked, because the
  host is little-endian.
- `tools/cross-count.sh`, `tools/count_dynamic.py` — dynamic
  instruction counts from real sparc64 cross-GCC output. Gated behind
  `-DINFER_VIS_COUNT` so the shipping build still inlines the kernels.
- `tests/t_viskern.c` covers Q6_K at 256/1024/3584 columns, with the
  256-column case deliberately misaligned.

### Measured and rejected

64-bit `ldx` loads (1395 → 1477), copy-based alignment normalisation
(5% fewer instructions, but 192 bytes of copy traffic per super-block
through a 16 KB direct-mapped cache), and 32-bit-accumulator rewrites
of Q4_K (261 → 280) and Q5_K (391 → 411). All reverted; see finding 28.

### Still unmeasured

Every figure here is an instruction count, not a cycle count. No
UltraSPARC hardware has run this code. On an in-order machine with a
16 KB direct-mapped D-cache these diverge, and this project has been
wrong by 2–3× in both directions from counts and emulation before.
`gmake solaris-vis-profile` on the target is the only thing that
settles it.

## 1.14.2 — the GCC targets actually call GCC

`gmake solaris-gcc-vis-profile` invoked **Sun Studio**, not GCC:

```
cc: Warning: Option -d=gnu89 passed to ld, if ld is invoked, ignored otherwise
cc: -W option with unknown program all
```

The Makefile had `CC ?= cc`, but make sets `CC = cc` as a built-in
default before the Makefile is read, so `?=` never fired. On Linux `cc`
is GCC and nothing looked wrong; on Solaris `cc` is Studio, and every
`solaris-gcc-*` target has been feeding GCC flags to the wrong compiler
since those targets were added.

- new `GNUCC = gcc`, used only by the five `solaris-gcc-*` targets
- `$(CC)` still drives x86 and generic tests; `$(SUNCC)` still Studio
- override for a GCC that is not on `PATH`:
  `gmake solaris-gcc-vis GNUCC=/usr/sfw/bin/gcc`

Also fixed: `--help` hardcoded `matvec kernel: auto, mmx, i8, ref`, so a
VIS build never listed `vis`. Now conditional on `INFER_HAVE_VIS`.
`--backend list` was always right.

Verified with a stub `cc` that rejects GCC flags the way Studio does,
placed ahead of a real GCC on `PATH`; `solaris*` routes to `cc` and
`solaris-gcc*` to `gcc`. The previously failing target now builds
warning-free and reports `vis ok ... <- selected`.

## 1.14.1 — the Sun Studio path actually compiles

1.14.0 claimed dual compiler support for the VIS backend. The Sun Studio
half had never been compiled by a Sun Studio, and did not compile. It
was written against an API that does not exist:

```c
typedef vis_f32 vis_u8x4;    /* no such type in <vis_proto.h> */
typedef vis_d64 vis_s16x4;   /* nor this one */
```

Sun declares the VIS intrinsics over plain `float` and `double` — a VIS
register is an FP register, and there are no vector types at all. Fixed
in both `src/backend_vis.c` and `tests/t_vis.c`:

- `vis_u8x4` is `float`, `vis_s16x4` is `double` on the Studio path
- `vis_hsum16()` and `vis_zero16()` gained a Studio implementation;
  lanes come out of a union rather than `v[0]`, which a `double` does
  not support
- dropped `vis_to_float()` / `vis_read_hi()`, which are mediaLib
  helpers and may not be in Studio's header, in favour of unions
- the Studio branch of the lane-ordering check was a stub that always
  printed `ok`; it now runs

The GCC path is unchanged and re-verified.

**Why `make solaris` succeeded anyway:** it does not compile
`backend_vis.c` and does not define `INFER_HAVE_VIS`. It builds a
scalar i8 binary and never sees the VIS code. It now says so.

### New

- `tests/t_viskern.c` — checks both VIS kernels against `qmv_i8` on
  synthetic blocks, no model required. Currently bit-identical
  (`0.000e+00`) for Q4_K and Q5_K at three sizes each.
- `tests/vis_shim/vis_proto.h` — reimplements Sun's exact prototypes in
  plain C so the Studio code path can be compiled *and run* under GCC
  with `-D__SUNPRO_C`. Catches type errors without a Sun Studio. It
  says nothing about Studio's code generation.
- `make solaris-gcc-vis-test`, matching the Sun Studio target.

### Changed

- `-xarch=v9a` → `-xarch=sparcvis -xvis` (Studio deprecated the former;
  `SUNFLAGS` already carries `-m64`).

### Note on the reference

`t_viskern` compares against `qmv_i8`, not `qmv_ref`. Against the exact
F32 reference the VIS kernels differ by 1–12%, which is int8 activation
quantisation, not a kernel bug — the i8 backend shows the same.

## 1.14.0 — VIS backend for UltraSPARC

A fourth compute backend, `vis`, using the UltraSPARC Visual Instruction
Set. Opt-in and separate from everything x86, exactly as `mmx` is.

```sh
gmake solaris-vis          # Sun Studio
gmake solaris-gcc-vis      # GCC
gmake solaris-vis-test     # prove the assumptions before trusting it
```

### The trick that makes it work

VIS is a *graphics* instruction set, and its partitioned multiply is
defined for pixel blending:

```
fmul8x16(u8 a, s16 b)  ->  (a * b + 0x80) >> 8
```

Taken at face value that is useless here. Our products are tiny — a
Q4_K nibble times an int8 activation is at most 15 x 127 = 1905 — so
a `>> 8` would discard nearly everything.

But the shift can be cancelled. Pre-shift the activation left by 8:

```
fmul8x16(w, x << 8) = (w * (x << 8) + 0x80) >> 8 = w * x
```

**exactly**, with no rounding error, as long as `x << 8` fits a signed
16-bit lane. Activations are quantised to [-127, 127], so `x << 8`
spans [-32512, 32512] and fits.

Verified exhaustively on the target: all 16320 combinations of weight
0..63 against activation -127..127, zero mismatches. `tests/t_vis.c`
re-runs that check on any new machine, and needs no model.

The pay-off is that a graphics instruction becomes an exact four-lane
integer multiply — the same width as MMX's `pmaddwd` — instead of
falling back to `fmuld8ulx16` at two lanes.

### Accumulator bounds

Products stay in 16-bit lanes, so each format can only take so many
terms before overflow. The kernels are written to these limits:

| format | max weight | terms per lane |
|---|---|---|
| Q4_K | 15 | 16 (16 x 15 x 127 = 30480) |
| Q5_K | 31 | 8 (8 x 31 x 127 = 31496) |
| Q6_K | 32 | 8 (8 x 32 x 127 = 32512) |

Widening to 32 bits uses `fmuld8ulx16` against a constant 1, which is
an exact two-lane 16→32 expansion and avoids an unpack VIS does not
have.

### Written for the pipeline, from the datasheet

UltraSPARC-II/IIi (Sun STP1031): 4-way superscalar, **two graphics
execution units**, the FPU issues two instructions per cycle, most VIS
ops are fully pipelined at throughput 1 and **latency 3**, and there
are **32 FP registers**.

Two consequences shaped the code. Latency 3 at throughput 1 means one
dependent accumulator chain runs at a third of peak, so the kernels keep
four independent accumulators and fold only at the end. And 32 registers
is why this is worth doing at all: the equivalent multi-row kernel on
MMX measured **0.90x — slower** — because with 8 registers only three
scratch remained and weights were re-loaded four times. That constraint
does not exist here. There is also no `EMMS`; VIS shares the FP register
file.

### Two bugs found while building it, both silent

**Vectors were spilling to the stack.** Building a vector element by
element (`v[0]=..; v[1]=..`) looks equivalent to a load but compiles to
four scalar stores and a reload. The first kernel did 124 memory
operations for 8 multiplies — spilling straight past the register file
the whole design depends on. Fixed by loading through a union of the
vector type and a 64-bit integer: 752 → 568 instructions.

**A cache that always hit.** `xs_prepare()` cached the pre-shifted
activations keyed on `(xq->q, xq->n)`, assuming consecutive matvecs in a
layer share an activation vector. But `bk_quantize_x()` returns a
pointer into a single *static* buffer, so `xq->q` is the same address
every call while the contents change underneath. The cache always hit
and every matvec after the first used stale data.

It passed `t_backend` — which calls matvec once per tensor — and
produced fluent-looking garbage in real generation. The cache is gone;
the shift is ~0.1% of the work for a 1024x1024 matrix and not worth
risking correctness for.

### Verified

* `t_vis`: 16320 exactness checks, lane ordering, accumulator bounds
* `t_backend` on SPARC: `rel-l2` and `max-rel` **bit-identical to i8**
  for both Q4_K and Q5_K
* end to end: `vis` and `i8` produce the same text under greedy
  decoding — `The capital of France is **Paris**.`
* x86 targets untouched; all four still build with zero warnings and
  every test passes

### Honest limits

Q6_K, Q8_0 and Q4_0 have no VIS kernel yet and fall through to the
portable `i8` path, so correctness is never at risk from a missing
kernel. Q6_K is 27% of runtime and is the obvious next target; its -32
bias makes weights signed, which needs care with `fmul8x16`'s unsigned
first operand.

**No speedup is claimed.** Everything above was measured under
emulation, which models VIS semantics but not timing. A marginal-cost
comparison under qemu put `vis` at 0.40x the work of `i8`, but this
project has a documented history of emulated and isolated measurements
being wrong by 2-3x in both directions. The number that matters is a
profile from real hardware:

```sh
gmake solaris-vis-profile
./infer-1.14.0-solaris-profile run model.gguf \
    -p "The capital of France is" -n 10 -t 0 --seed 1 -c 512 \
    --backend vis --log-perf --log-stages --log-file sparc-vis.txt
```

---

## 1.13.0 — prompt-prefix cache

`chat` and `serve` re-render the whole conversation every turn, then
pushed all of it through the network again. The engine already had a KV
cache; what it lacked was any way to say "you have seen this prefix
before".

`qwen35_reuse_prefix()` adds that. On a hit the caller skips the shared
tokens entirely and decodes only what is new.

### The constraint that shapes the design

Only 6 of the 24 layers are attention layers with a position-indexed KV
cache that could be truncated at will. The other **18 are Gated
DeltaNet**: a recurrent state that has absorbed every token in order and
**cannot be rewound**. Snapshotting per token is not an option either —
at n_v_heads x 128 x 128 floats per layer that is roughly 18 MB per
token.

So reuse is all-or-nothing: the entire history must be a prefix of the
new prompt. Anything else resets, which is exactly the old behaviour.

`tests/t_cache.c` asserts the property that matters — a prompt decoded
incrementally produces **bit-identical logits** to one decoded fresh
(max difference 0.000e+00) — plus the rejection cases: an edited
history, a shorter prompt, and a reset all correctly refuse to reuse.

### Tokenisation had to change too

The first working version still never hit in chat. The cause is worth
recording: byte-level BPE merges greedily, so text the model *generated*
as several tokens merges differently when it reappears inside a longer
string. `"\n"` (198) becomes part of `"\n\n\n\n"` (14200) — identical
text, different token IDs, cache never hits.

`agent_tokenize_incremental()` fixes this by splitting at the previous
prompt's boundary and tokenising the two parts separately, so the shared
prefix keeps byte-identical IDs.

### What it actually buys

Measured on `/v1/completions` with an appending prompt: **15 of 19
tokens reused**, then 23 of 26. That is the case it was built for and it
works.

**Chat with the stock Qwen template does not hit**, and this is not a
bug in the cache. The template rewrites history between turns: a reply
generated as `<think>...</think>Hello` is re-rendered on the next turn
as just `Hello`. The token sequences genuinely differ, so resetting is
correct. Verified by dumping both prompts and diffing them.

The cache therefore helps:

* `/v1/completions` and any appending workload — fully;
* agentic loops that append tool results without rewriting history;
* chat with a template that does not rewrite history;
* the server's own tool loop, where the same prompt is rendered twice.

and does nothing for stock-template chat. Stated plainly rather than
claimed as a general speedup.

### Also

* `-v` reports `prompt cache: reused N of M tokens` on a hit, and on a
  miss says which token diverged — the diagnostic that found the BPE
  problem above.
* MCP tool loop re-verified end to end with the cache active: correct
  answer, arguments intact.

---

## 1.12.2 — working CI, automatic release builds, agent guidance

### CI was broken in two ways

**Missing package.** The workflow installed `gcc-mingw-w64-i686-win32`
but not `binutils-mingw-w64-i686`, which is what provides
`i686-w64-mingw32-objdump`. Every Windows verification step would have
failed with "command not found".

**Hardcoded version.** Eleven literal `1.12.1` strings referred to
binaries by name. Bumping `VERSION` broke the whole workflow. The
version is now read once from `make version` into `$GITHUB_ENV`, so a
bump cannot break CI.

A third problem was found while testing the fix: the "confirm the win32
mingw variant" step grepped for `thread-model=`, which **this Debian
mingw does not print at all** — the check would have aborted every run
under `set -e`. Replaced with a test of the property that actually
matters: compile a trivial program and confirm it does not import
`libwinpthread-1.dll`.

Every step was then executed locally, verbatim, before committing.

### Automatic release builds

One workflow, two jobs. `build` runs on every push; `release` runs only
for a `v*` tag, and instead of rebuilding it **downloads the artifact
`build` just verified**. The released binaries are therefore
byte-for-byte the ones that passed every check.

A second workflow file was tried first and discarded: it duplicated the
toolchain setup and the whole verify block, and two copies of a
verification list drift. `needs: build` expresses the real dependency
and keeps the logic in one place.

To cut a release:

```sh
git commit -am "1.12.2"
git tag v1.12.2
git push && git push --tags
```

That is the whole procedure. The workflow:

Guards, each of which exists because the failure is silent otherwise:

* the tag must match `VERSION` — a release whose tag and binaries
  disagree is worse than no release;
* `make checkversion` — the Makefile and `src/infer.h` must agree;
* **the tagged commit must carry that version** — `git archive` packages
  the commit, so tagging before committing a version bump would ship a
  source zip whose `infer.h` contradicts the binaries next to it;
* the full test suite and platform checks run before anything is
  published.

The release ships `infer-<version>-binaries.zip` with `SHA256SUMS`, plus
a source archive. Artifacts lose the executable bit, so the job restores
it before packaging.

Every push also uploads the four binaries as a downloadable artifact, so
you can grab a build without cutting a release.

### Agent guidance

`AGENTS.md` at the repository root, plus `.agents/` with the detail:

| file | contents |
|---|---|
| `AGENTS.md` | build, verify, and a "never do these" list |
| `.agents/BUILDING.md` | the exact Windows XP recipe with expected output, and a failure table |
| `.agents/CONVENTIONS.md` | C89 rules, the no-64-bit-type rule, naming, ownership |
| `.agents/PITFALLS.md` | ten bugs that actually happened here, and how each was found |

The theme, which is what agents most often get wrong here: **the
expensive bugs in this project do not crash.** They build cleanly, load
the model, print correct metadata, run at full speed and return
confident nonsense. Compiling is not evidence of working.

---

## 1.12.1 — Solaris actually builds, and four targets instead of twenty

### Solaris: the real cause, and the real fix

1.12.0 failed on UltraSPARC with `invalid cast expression`. The first
attempt at a fix only moved the failure one line:

```
"src/sys_posix.c", line 59: argument #6 is incompatible with prototype:
    prototype: union {double *d, array[2] of int* l} : "/usr/include/sys/mman.h", line 168
    argument : long
```

That union is the whole story. In strict C90 mode `-Xc` undefines
`_LONGLONG_TYPE`, because C90 has no `long long`, and Solaris's
`<sys/types.h>` falls back to:

```c
typedef union { double _d; int32_t _l[2]; } longlong_t;
```

With `_FILE_OFFSET_BITS=64` on a **32-bit** build, `off_t` becomes that
union. Nothing can be cast to it, assigned to it, or passed to
`mmap()`. No amount of rewriting our own code helps.

**The fix is to stop building 32-bit.** These are 64-bit machines —
UltraSPARC IIi and later. In LP64 `off_t` is already a plain 64-bit
`long`, large-file support is automatic, and the union never appears.
The Solaris targets now pass `-m64` and no longer pass
`_FILE_OFFSET_BITS=64`.

`sys_posix.c` keeps the page-index arithmetic from the first attempt,
which is correct on both models and names no 64-bit type.

### Four build targets instead of twenty

The target list had grown to `all i486 geode windows windows-mmx profile
profile-i486 profile-geode profile-windows solaris solaris-gcc
solaris-profile solaris-gcc-profile solaris-fast solaris-test test bench
clean version checkversion` — most of them near-duplicates.

Now, for x86:

```
make linux              make windows
make linux-profile      make windows-profile
make all
```

**Every one contains all three backends and uses an i486 baseline.**
There is no longer a separate "MMX build" or "i486 build", because a
single binary already covers both: MMX is selected at run time from
CPUID, and nothing newer than a 486 is emitted for ordinary code. The
only remaining axis is the profiler.

Solaris/SPARC keeps its own section, deliberately isolated so neither
set of flags constrains the other.

`make help` prints the list. Old target names are gone rather than
aliased, so a stale script fails loudly instead of building something
unexpected.

### For AI agents: an exact Windows XP recipe

Several agents have failed to produce a working XP build. COMPILE.md
section 2 is now a literal, verified, top-to-bottom sequence: install,
confirm the `win32` (not `posix`) mingw variant, build, and the four
static checks that prove it will load on XP. Plus a table of the
mistakes that actually happen, including sandboxes discarding installed
packages between turns and the wasted effort of trying to run the `.exe`
under Wine.

### Earlier in this release

#### `sys_posix.c` would not compile with Sun Studio

Reported from a real UltraSPARC:

```
"src/sys_posix.c", line 50: invalid cast expression
cc: acomp failed for src/sys_posix.c
```

Line 50 was `off = (off_t) aligned;`. With `_FILE_OFFSET_BITS=64` on a
32-bit host, `off_t` is a **64-bit type** — `long long` on Solaris. C90
has no `long long`, so under `-Xc -xc99=none` a cast to it is a syntax
error, not a warning. GCC accepts it silently as an extension, which is
why every Linux build passed.

Fixed by never naming a 64-bit type in our source. The page-aligned
offset is kept as a `long` page *index* and multiplied by the page size
inside the `mmap()` call, so the compiler widens implicitly using
whatever `off_t` happens to be:

```c
page_index = (long) (offset / (double) pagesz);
...
base = mmap(NULL, maplen, PROT_READ, MAP_PRIVATE, fd, page_index * pagesz);
```

Verified with `gcc -std=c89 -pedantic -Werror=long-long -m32
-D_FILE_OFFSET_BITS=64`: clean. The only remaining occurrence of the
phrase "long long" in the codebase is the comment explaining this.

#### Solaris flags updated from field use

`-xO5` and `-xunroll=16` adopted from a user's own flag set: the 4-issue
in-order UltraSPARC pipeline benefits from unrolling and neither flag
changes results.

Two flags from that set are deliberately **not** default:

* **`-fast`** is a macro that enables `-fsimple=2` and `-fns` — non-IEEE
  float with reassociation and flush-to-zero. The k-quant scales are
  fp16 values near zero and the DeltaNet recurrence accumulates over 128
  steps, so this is a real correctness risk rather than a theoretical
  one. Available as `make solaris-fast` for anyone who wants to try it,
  with `./build/t_backend model.gguf` to check the error terms.
* **`-xvis`** enables VIS intrinsics. There is no VIS code in the
  project, so it has no effect. Harmless, just pointless.

#### CMOV: measured, not assumed

1.12.0 moved the MMX builds to a 486 baseline, which removes CMOV.
Measured cost of that on x86-64, MMX backend, identical work:

| build | best | median |
|---|---|---|
| `-march=i486` (0 CMOV) | 7.68 s | 7.70 s |
| `-march=geode` (30 CMOV) | 7.72 s | 7.72 s |

**0.6%, and in favour of the no-CMOV build** — i.e. noise. Of the 30
CMOVs the geode build emitted, only 3 were in a hot function
(`bk_quantize_x`, once per matvec); the rest were in template parsing,
server startup and `malloc` wrappers. There is nothing to gate: the 486
baseline is free.

---

## 1.12.0 — Solaris/SPARC support, and refusing to guess byte order

1.11.0 made the code byte-order neutral and verified it on s390x and
SPARC64 — both with GCC. A user then built it on a real UltraSPARC with
**Sun Studio 12.3** and got `!!!!!!!!!!` after eight minutes of
computation.

The code was correct. The *detection* was not.

### The bug

| | GCC / Clang | Sun Studio 12.3 |
|---|---|---|
| SPARC macro | `__sparc__` | `__sparc`, `__sparcv9` |
| byte order | `__BYTE_ORDER__` | not defined |
| unprefixed `sparc` | defined | **suppressed by `-Xc`** |

The `#if` tested `__sparc__` and `__BYTE_ORDER__`. Under `-Xc` on Sun
Studio neither exists, every condition was false, and the `#else`
selected little-endian — on a big-endian machine. Clean build, model
loaded, correct metadata, full speed, garbage output.

`1.0f` read byte-reversed is `4.6e-41`, so every quantisation scale in
the model became a denormal near zero.

### Fixed in two parts

**Detect properly.** Prefer `__BYTE_ORDER__`, then test every vendor
spelling: `__sparc`, `__sparcv8`, `__sparcv9`, `_BIG_ENDIAN`, `__hppa`,
`mc68000`, `_M_PPC`, plus an explicit little-endian list.

**Refuse to guess.** The `#else` is now `#error` with the remedy in the
message, so an unrecognised compiler fails to build rather than
silently choosing. And `gguf_open()` calls the new
`inf_check_byte_order()`, which compares `INFER_BIG_ENDIAN` against a
runtime probe and decodes a known bit pattern:

```
byte order mismatch: this binary was compiled for a little-endian host
but is running on a big-endian one.
  rebuild with -DINFER_BIG_ENDIAN=1
```

One second at startup, instead of eight minutes ending in noise.

### Solaris / SPARC build targets

```
make solaris              Sun Studio, auto-tuned to the build host
make solaris-gcc          GCC on Solaris
make solaris-profile      Sun Studio + stage timers
make solaris-gcc-profile  GCC + stage timers
make solaris-test         the test programs
```

These encode the two things that are easy to get wrong by hand:

* **`-lsocket -lnsl`** — Solaris keeps the socket API outside libc.
  Its absence is the most likely reason a GCC build failed to link
  where Sun Studio succeeded.
* **Sun Studio option spellings** — `-Xc -xc99=none` for strict C90,
  `-xO3`, `-xtarget=`. GCC's `-std=c89 -pedantic -O2` are not
  recognised by `cc`.

Cross-build for another machine with `make solaris SUNTARGET=ultra2`.

The portable `i8` backend is selected automatically on SPARC and is
**6.5–8.8× faster than `ref`** (measured per weight format under
emulation). There is no VIS backend yet.

### Versioned executables

Every binary now carries its version, so several builds can share a
directory without being renamed by hand:

```
infer-1.12.0            infer-1.12.0.exe
infer-1.12.0-i486       infer-1.12.0-mmx.exe
infer-1.12.0-geode      infer-1.12.0-profile.exe
infer-1.12.0-profile    infer-1.12.0-i486-profile
infer-1.12.0-geode-profile
```

`make version` prints it, `make checkversion` asserts it matches
`src/infer.h`, and `make SUFFIX=` restores the plain unversioned name.

### Makefile is POSIX again

The version was briefly extracted with `$(shell ...)`, which is a GNU
make extension that Solaris `/usr/ccs/bin/make` does not implement — it
would have produced binaries named `infer-`. Now a literal, guarded by
`make checkversion`. `:=` was likewise replaced with `=` throughout.

### MMX builds now have a 486 baseline

`make geode` and `make windows-mmx` were compiled `-march=geode`, which
let GCC emit **CMOV throughout ordinary code** — `xmalloc`, `gguf_open`,
`main`. CMOV is a Pentium Pro instruction; those binaries would fault on
a 486 or original Pentium despite the MMX kernels being runtime-gated.

Both now use `-march=i486 -mtune=geode -mmmx`: baseline 486, Geode
*scheduling*, MMX confined to `backend_mmx.c` behind its CPUID check.
Verified 0 CMOV instructions in our code, 354 MMX instructions still
present, both backends still bit-identical under greedy decoding.

**One binary now runs on a 486 and accelerates on a Geode**, which makes
the separate non-MMX build redundant for most users.

Honest caveat: mingw's C runtime still uses CMOV in `strtod`, `pow` and
`ldexp`, all of which we call. The Windows builds are therefore
Pentium-Pro-and-later in practice even though our own code is
486-clean. Documented in COMPILE.md rather than papered over.

### No bundled chat template

`templates/qwen-fixed-v21.3.jinja` has been removed. Third-party
templates are their authors' work to distribute; `templates/README.md`
now points at the source with a `curl` command instead.

### Documentation

`COMPILE.md` and `docs/ARCHITECTURE.md` rewritten in full. Every build
step is now a literal copy-pasteable command with its expected output,
including the verification commands for each target (486-purity, CMOV
count, DLL count, MMX constant alignment). ARCHITECTURE.md gains a
request walkthrough, the qwen35 hybrid layout, the k-quant block format,
and why the integer kernels are shaped the way they are.

### Also

* `t_endian` asserts `inf_check_byte_order()`
* Finding 20 in `docs/FINDINGS.md`

No engine, kernel or tokeniser changes. `t_engine` output is unchanged
and x86 performance is unaffected.

---

## 1.11.0 — runs on big-endian hosts

`infer` now produces bit-identical results on little- and big-endian
machines. Verified on real big-endian hardware emulation (s390x under
qemu), not by inspection.

Before: the s390x build loaded the model, printed correct metadata, and
generated `!!!!!!!!`.
After: `The capital of France is **Paris**.` — the same tokens x86 gives.

### The bug was not where the audit said it was

A source audit found three sites that read little-endian file bytes
through a host-order `memcpy` or pointer cast, and predicted those were
the whole problem. They were real and are fixed:

* `gguf.c` `rd_f32` / `rd_f64` — metadata scalars
* `quant.c` `dot_f32` and the F32 branch of `q_dequant_row`
* `qwen35.c` `f32data()` — F32 norm/conv/SSM tensors, 9 call sites

But fixing all three still produced garbage. The actual culprit was in
`q_fp16_to_fp32()`, a function the audit had explicitly cleared as
"pure integer bit math, endianness-neutral":

```c
unsigned long bits;        /* 8 bytes on LP64 */
...
memcpy(&out, &bits, 4);    /* copies the HIGH half on big-endian */
```

It is not a byte-order bug at all — it is a **type-width** bug that only
becomes visible on big-endian. On a 64-bit big-endian host every fp16
value decoded to `0.0`, so every quantisation scale in the model was
zero. The file parsed, the tensors loaded, nothing errored, and the
output was noise. Fixed by narrowing to a 32-bit object before the copy.

### Cost: none

| | 1.10.0 | 1.11.0 |
|---|---|---|
| end-to-end, x86-64 i8 | 11.96 s | 12.00 s |

0.35%, i.e. noise. The hot path never changed: the k-quant kernels
already read weights a byte at a time, and `rd_f16p` — the most-called
wide read in the program — was already `p[0] | (p[1] << 8)`. F32 data is
0.07% of the weights in a k-quant model.

The i486 constraint had accidentally bought most of this: a 486 has no
wide loads worth using, so the expensive code was byte-wise already.

One near-miss worth recording: the first fix extracted the four bytes by
hand, which was correct but stopped the compiler folding the copy into a
single `movd` and cost **4% end-to-end**. The final version keeps the
one-instruction codegen on both byte orders.

### Also

* `INFER_BIG_ENDIAN` autodetected in `infer.h`, overridable with
  `-DINFER_BIG_ENDIAN=0/1`
* `inf_rd_f32p` / `inf_rd_f64p` / `inf_rd_f32v` in `util.c`
* `gguf_f32data()` — returns the mapping directly on little-endian; on
  big-endian makes one swapped copy per F32 tensor at first use (1.9 MB
  for this model), because the blob is mapped read-only
* `tests/t_endian.c` — 13 assertions, host-independent
* CI cross-compiles for s390x and runs the endian test under qemu

No engine, kernel, tokeniser or MMX code changed. `t_engine` output is
identical and the Geode is unaffected.

---

## 1.10.0 — one think switch, opt-in logging, chat timings

### `--think [on|off]` replaces `--think` / `--no-think`

Two flags for one boolean meant both could be passed at once, with the
outcome decided by argument order. Now there is one switch: `--think`
alone means on, `--think off` is the explicit form.

The value is optional, and the parser only consumes the next word when
it really is one — `--think --raw` sets thinking and leaves `--raw`
alone. A bad value names itself instead of blaming the wrong word:

```
$ infer chat model.gguf --think banana
'--think' expects on or off, not 'banana'
```

`/think on|off` in the REPL is unchanged.

### Logging is opt-in again, including in profile builds

**Fixed:** `infer-profile.exe` printed a full stage profile and a perf
block after every request whether or not they were asked for. `main.c`
had `prof_perf_enabled = log_perf || PROF_ENABLED`, so compiling with
`-DINFER_PROFILE` forced reporting on. It has behaved this way since
1.2.0, when the profile builds were introduced.

Building for measurement and asking for measurements are now separate:

```
--log-perf      TTFT, tokens/second, seconds/token per request
--log-stages    the per-stage profile at exit
```

A `-DINFER_PROFILE` build is silent until one of them is given. Normal
builds still contain no stage timers at all, and `--log-stages` there
says so rather than doing nothing:

```
$ infer run model.gguf --log-stages
--log-stages needs a build with -DINFER_PROFILE (make profile /
profile-geode / profile-windows); this build has no stage timers
compiled in
```

### `chat` reports speed

`chat` had no timing instrumentation at all. With `--log-perf` it now
prints tokens/second and seconds/token after every turn, and a session
total on exit:

```
--- perf: chat session (2 turns) ---
  prompt      :    48 tok in 14.54 s   (3.302 tok/s, 0.30 s/tok)
  generation  :    16 tok in 6.91 s    (2.316 tok/s, 0.43 s/tok)
  total       : 21.45 s
```

`/perf` shows the running total mid-conversation, and `/set` now lists
whether logging is on. Because every turn re-ingests the conversation,
the per-turn prompt figure grows with history — visible evidence for
when `/forget` is worth using.

### Also

* Fixed a 48-byte leak in the Jinja subscript evaluator
  (`jinja_eval.c`), found with AddressSanitizer. It allocated a `none`
  fallback eagerly and overwrote it on every successful lookup.
  Pre-existing, one allocation per process, not per turn.
* `t_agent` grew to 18 assertions covering `--think` semantics and the
  opt-in logging switches; CI checks that `--no-think` is gone and that
  a profile build stays quiet.

---

## 1.9.0 — options are general, not per mode

Every toggle now works in every mode where it can mean something.
`--system`, `--think`, `--no-think`, `--raw`, `--template`, `--mcp`,
`--tool-rounds` and `--quiet-tools` apply to `serve`, `chat` **and**
`run` alike; previously they were parsed everywhere but read only by
`chat`, so using them elsewhere did nothing and said nothing.

### One option table (`src/opts.c`)

Each option is declared once, with the set of modes it applies to. That
declaration drives parsing, the generated `--help`, and an applicability
check, so the three can no longer disagree.

```
$ infer run model.gguf --port 9090
'--port' has no effect in run mode (it applies to: serve)
```

Only five options are restricted, each for a concrete reason:
`--host`, `--port`, `--api-key`, `--alias` (nothing else opens a socket)
and `-p/--prompt` (`serve` takes its prompt from the request body).

### Shared prompt and tool layer (`src/agent.c`)

Prompt rendering and tool calling moved out of `chat.c`. All three modes
call the same code, so they cannot drift; a test asserts that a
conversation built by `chat` and the same one arriving as JSON at
`serve` render to byte-identical prompts.

### What each mode gained

**`run`** — `--system`, `--think`, `--template`, `--mcp` and the full
tool loop. It renders the real chat template instead of the hand-written
ChatML string it used before.

**`chat`** — `-p` seeds the first turn before the REPL opens; `--raw`,
`--tool-rounds`, `--quiet-tools`; `/set` and `/raw on|off` commands.

**`serve`** — `--system` and `--think` as per-request defaults,
`--raw`, and `--mcp` with a **server-side tool loop**: the server
executes tools itself and returns only the final answer, so an ordinary
OpenAI client gets tool use without knowing it. A request carrying its
own `"tools"` array takes over instead. Requests may override with
`"raw"`, `"think"` and `"enable_thinking"`.

### Two bugs found while testing

**JSON-form tool calls were parsed as empty.** Qwen3.5 emits tool calls
in two shapes; only the XML one was handled. The JSON form still
"matched", found no `<parameter=` tags, and produced an empty argument
object — so the tool ran with no arguments and returned a
plausible-looking wrong answer, with nothing in the logs to indicate a
parse failure. Both forms, the OpenAI `{"function":{…}}` nesting and
double-encoded arguments are now handled and tested.

**The server discarded most of each message.** Only `role` and
`content` were copied out of incoming messages, so templates reading
`message.tool_calls` or `message.reasoning_content` silently lost them
and tool-using conversations could not be replayed through the API.
Whole messages are now converted.

### Also

* `--no-think` to switch thinking off explicitly
* `--tool-rounds <n>` (default 4) and `--quiet-tools`
* `tests/t_agent.c` — 10 assertions covering cross-mode prompt
  equivalence and every tool-call shape
* CI asserts the option matrix: every general flag accepted in every
  mode, and inapplicable ones rejected rather than ignored

No engine, kernel or tokeniser code was touched; `t_engine` output is
unchanged and performance is unaffected.

---

## 1.8.0 — custom templates, and full documentation

### `--template <file>`

Load a Jinja chat template from disk instead of the one baked into the
GGUF. Works for both `chat` and `serve`.

```sh
infer chat model.gguf --template qwen-fixed.jinja
```

Verified against
[froggeric/Qwen-Fixed-Chat-Templates](https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates)
— a 16 kB community template, twice the size of the built-in one.
Rendering it with `infer` and with Python Jinja2 produces
**byte-identical** output for plain chat, for tool-enabled prompts, and
with thinking on and off.

Three Jinja features were added to support it:

* **`|join(sep)`** filter (also `|int`, `|abs`)
* **general slicing** `seq[a:b]` with negative and omitted bounds — the
  engine previously handled only `[::-1]`
* **`for k, v in d.items()`** two-variable unpacking over pair lists

### The server now renders Jinja too

`/v1/chat/completions` previously hand-wrote the ChatML layout in C —
correct for Qwen3.5, wrong for anything else. It now renders the model's
own template, or a `--template` override, exactly as `infer chat` does.
Both paths produce identical prompts.

### Documentation

* **`USER-GUIDE.md`** — every command and flag with worked examples
* **`COMPILE.md`** — rewritten as atomic steps for every target,
  including verification commands for 486-purity, XP DLL imports and
  MMX constant alignment
* **`docs/ARCHITECTURE.md`** — data flow, layering, ownership rules
* **`docs/FINDINGS.md`** — 15 findings and oddities, each with the
  measurement behind it
* **`docs/TEMPLATES.md`** — supported Jinja, and how to debug a template
* **`docs/files/`** — one document per source file, 20 in total
* `docs/PERFORMANCE-ANALYSIS.md` moved under `docs/`
* `imgs/example.jpg` linked from the README

---

## 1.7.0 — chat, real Jinja templates, MCP tools

### `infer chat`

An interactive multi-turn REPL. Keeps the conversation, re-renders it
through the model's own template each turn, streams the reply. Commands
for `/history`, `/system`, `/forget`, `/think`, `/tools`, `/prompt` and
`/stats`. See **CHAT.md**.

### Real Jinja2 template rendering

Previously the ChatML layout was hand-written in C — correct for
Qwen3.5, wrong for anything else. There is now a Jinja subset
interpreter (`jinja.c`, `jinja_eval.c`, ~1200 lines of C89) that renders
`tokenizer.chat_template` directly from the GGUF.

Covers `if`/`elif`/`else`, `for` with the `loop` object, `set` (inline
and block), `macro` with defaults and kwargs, `namespace()`, 11 filters,
9 tests, the full operator set including ternaries, string methods,
`[::-1]` slicing and `raise_exception()`.

**Verified byte-identical to Python Jinja2** on the real Qwen3.5
template — plain chat and tool-enabled, thinking on and off.

Two bugs found and fixed while getting there, both of which silently
corrupt prompts:

* `{{-` was trimming whitespace that an *expression* had just emitted,
  and reaching back into the previous loop iteration. It must only trim
  literal template text, and never past the start of the current
  iteration. This was deleting the newline after every chat message.
* `|tojson` must **sort object keys** (Jinja2 uses
  `json.dumps(sort_keys=True)`). Without it the tools prompt differed
  byte-for-byte from every other runtime.

### MCP tool support over HTTP

`--mcp http://host:port/path` connects to an MCP server using the
Streamable HTTP transport: `initialize`, `notifications/initialized`,
`tools/list`, `tools/call` over JSON-RPC 2.0. Discovered tools are fed
into the template's `tools` variable, and tool calls are executed and
returned to the model for up to 4 rounds per turn.

Plain-JSON and SSE responses are both handled, as is `Mcp-Session-Id`.

stdio transport is **not** implemented (no portable ANSI C way to spawn
children and wire pipes). https is **not** supported (no TLS stack).

`net.h` gained one function, `net_connect()`, implemented for both POSIX
and Win32 using `gethostbyname()` rather than `getaddrinfo()` so it
still works on Windows XP and earlier.

### Verification

* `t_jinja`: 25 expression/statement cases plus the real template from a
  model file; byte-compared against Python Jinja2 offline.
* Tool-call parser unit-tested against 6 shapes including a missing
  `<tool_call>` wrapper and duplicate parameter names.
* Multi-turn memory confirmed end-to-end; MCP tool call confirmed
  end-to-end against a test server.
* 9 build targets, zero warnings; `i486` still emits no MMX; both `.exe`
  still 3 DLLs with no post-XP API; `run` and `serve` unchanged.

---

## 1.6.0 — the recurrence, and a reverted regression

### What 1.5.0's data actually said

The 1.5.0 Geode log contained a sharp signal that was not expected:

| format | instrs cut | time cut |
|---|---|---|
| Q5_K | 52 -> 26 (50%) | **21%** |
| Q4_K | 20 -> 16 (20%) | **1%** |

Both got instruction reductions; only one got faster. The difference is
*which* instructions. Q5_K's cut removed re-**loads** (it was re-reading
and re-widening the same weight bytes four times per chunk). Q4_K's cut
removed register-only ALU ops.

Dividing time by memory accesses gives the same answer for every format:

```
Q4_K  8.0 cyc/MAC -> 25.6 cycles per memory access
Q5_K  9.4 cyc/MAC -> 25.1 cycles per memory access
Q6_K  9.8 cyc/MAC -> 26.1 cycles per memory access
```

~25 cycles per access, flat across formats with very different ALU
loads. **The kernels are bound by memory accesses, not arithmetic.**

### Multi-row: tried, measured, rejected

The obvious response is to reuse each activation load across several
output rows — the matvec re-reads the whole activation vector once per
output row.

I built a correct 2-row Q4_K kernel (verified bit-identical to the
1-row version) and benchmarked it against a realistic streaming weight
buffer. It came out **0.90x — slower.**

The reason is register pressure. MMX has 8 registers; with `mm7` as the
zero constant and four accumulators (2 rows x lo/hi nibble), only three
remain. That is not enough to keep both rows' unpacked nibbles live, so
the 2-row kernel re-loads each weight byte four times. It trades 4 saved
activation loads for 6 extra weight loads.

Recorded here so nobody tries it again: **multi-row is an SSE2
optimisation (16 registers, 8 lanes), not an MMX one.**

### What did work: fusing the delta-net recurrence

With matvec at 91% of runtime it is easy to ignore the rest, but the
state recurrence had grown to **5.7%** and had never been touched.

Per head, `S` is 128x128 floats = **64 KB — exactly the Geode's L1 data
cache**. The code made four separate full passes over it (decay, delta,
rank-1 update, readout), so S was streamed through L1 four times per
head, 16 heads x 18 layers per token.

Passes 1+2 are now fused (decay a row, then immediately dot it with k)
and passes 3+4 are fused (update a row, then dot it with q). Each row is
touched while still hot; S traffic halves.

Measured on the reference host: **0.290 s -> 0.191 s, 1.52x** on that
stage. Cache effects are understated on a machine with a 32 MB L3, so
the Geode should do at least as well.

The arithmetic is unchanged and the output is **bit-identical** to
1.5.0, verified over a 40-token greedy generation.

### Reverted: i8 Q4_K lookup tables

1.5.0 added nibble lookup tables to the portable backend on the strength
of a 2.46x isolated microbenchmark. The Geode i8 log shows what they
actually did:

| | 1.4.0 | 1.5.0 |
|---|---|---|
| Q4_K | 313.3 s | **322.5 s** (3% *worse*) |
| Q5_K | 239.0 s | 236.8 s |
| Q6_K | 233.0 s | 171.4 s |

Q4_K got slower. The tables are now used only by Q5_K, whose inner loop
already loads the `qh` byte so the extra table load rides along with a
load that was happening anyway.

Lesson recorded in the source comment: **a microbenchmark with
everything in L1 does not predict this machine.** It cost a release to
learn.

### Verification

* 2,244 comparisons (320 tensors x 6 activation patterns x 2 backends)
  against the scalar reference: zero failures, worst relative L2
  1.219e-02, unchanged.
* Output **bit-identical to 1.5.0** across a 40-token greedy sample —
  important, because the recurrence change touches model maths, not just
  a kernel.
* 9 build targets, zero warnings; `qwen35.c` and `backend.c` both still
  strict C89; `i486` emits no MMX; all three `.exe` still 3 DLLs with no
  post-XP API and `.rdata` 8-byte aligned.

### Where this leaves things

Per forward pass on the Geode (1.5.0 figures):

| | share |
|---|---|
| matvec (all formats) | 91.1% |
| state recurrence | 5.7% -> now ~3.8% |
| causal conv + SiLU | 1.5% |
| everything else | 1.7% |

The matvec kernels are at ~25 cycles per memory access and ~3.2 cycles
per instruction against a documented ~2-cycle pipeline throughput. There
is no large arithmetic win left in MMX. What remains:

1. **A smaller model or quantisation** — the only lever with multiples
   left in it. Q4_K_S, or a 0.4B-class model.
2. **Batched prompt ingestion** — TTFT only, but large (3.1 min now).
3. **Requantising `token_embd`** — the LM head is 18.1% in 11 calls.

---

## 1.5.0 — fewer instructions per multiply

### What 1.4.0 taught us (negative result)

1.4.0 batched `EMMS` 16x and rescheduled Q4_K to break dependency
chains. On the Geode that bought **1.04x** — I had predicted "1.1x or
much more". That is a useful negative result: **EMMS and dependency
stalls were not the dominant cost.**

Disassembling the 32-bit build showed why. In the hot loop:

```
pmaddwd  :  56   <- the only instruction doing real work
movq     : 231   <- 4.1 data moves per multiply
pand     : 104
psrlw    :  76
psllw+paddw: 96  <- Q5_K hi-bit insertion
punpck   :  64
MMX total: 730 for 56 pmaddwd  = 13 instructions per multiply
```

Working back from the Geode's own numbers: ~1936 M MMX instructions per
forward pass against a 6186 M cycle budget = **3.2 cycles per MMX
instruction**, versus OLPC's measured pipeline throughput of ~2. The
chip was already close to its issue limit. It was not stalling — it was
being asked to execute far too many instructions.

So 1.5.0 attacks instruction count, not scheduling.

### Byte-domain nibble masking (MMX)

The kernels widened bytes to 16-bit words *first* and then masked each
half separately. Masking in the **byte domain** before widening lets one
`pand` handle all eight nibbles at once:

| | before | after |
|---|---|---|
| Q4_K, per 16 weights | 20 instr | 16 instr |
| Q5_K, per 16 weights | 52 instr | 26 instr |

Q5_K was the worst offender — it re-loaded `qs`, re-widened it, and
redid the hi-bit shift/mask/shift for every one of its four `pmaddwd`.

Measured kernel throughput (32-bit build, Mflop/s):

| format | 1.3.0 | 1.4.0 | 1.5.0 |
|---|---|---|---|
| Q4_K | 4279 | 5125 | **5425** |
| Q5_K | 2847 | 2847 | **3953** |
| Q6_K | 2734 | 3206 | 3206 |
| Q8_0 | 2451 | 2591 | 2591 |

Q6_K was left alone deliberately: its values carry a −32 bias, which
makes them signed, and `punpcklbw` against a zero register
zero-extends. Byte-domain masking there needs careful sign handling and
the risk of a silent numeric error outweighed the ~25% of runtime it
represents.

### Nibble lookup tables (portable i8 backend)

Two 256-byte tables map a packed byte to its low and high nibble,
replacing an AND and a SHIFT per weight with two loads. 512 bytes total
— permanently resident in even a 486's 8 KB L1.

On an isolated 486-targeted benchmark this is **2.46x** on the Q4_K
inner loop. In the full kernel the gain is smaller but real (Q4_K and
Q5_K both improve); Q6_K measured *slower* with tables, so it keeps the
arithmetic path. This needs no SIMD and helps the plain `i486` build.

### Measured end-to-end

Best-of-2, 32-bit builds, on an out-of-order reference host:

| build | 1.4.0 | 1.5.0 |
|---|---|---|
| i486 / i8 | 22.6 s | 21.3 s |
| geode / mmx | 9.1 s | 8.4 s |

**Caveat as always:** the reference host is out-of-order with a fast
ALU and a large L1, which systematically under-reports changes aimed at
in-order hardware. Track record: 1.3.0 measured 1.72x here and **2.08x**
on the Geode. Run-to-run noise on this shared machine is ±15%, larger
than several of the deltas above, which is why the per-kernel table is
the more trustworthy measurement.

### About branch prediction

It was suggested the Geode LX has no branch predictor. **It does**, and
two independent measured sources confirm it:

* OLPC, benchmarking real XO-1 silicon: *"The IU has a branch predictor
  of an unspecified size"*
* 7-cpu.com, measured on a Geode LX 800: *"Branch misprediction penalty
  = 8 cycles"*

A misprediction penalty cannot exist without a predictor. And the
arithmetic bounds it regardless: the matvec loops execute ~56 M branches
per forward pass, so even at an implausible 25% miss rate at 8 cycles
that is **1.8% of the cycle budget**. Branches are not where the time
goes — 13 MMX instructions per multiply was.

### Verification

* 2,244 comparisons (320 tensors x 6 activation patterns x 2 backends)
  against the scalar reference: **zero failures**, worst relative L2
  1.219e-02, unchanged from 1.3.0.
* All four build/backend combinations still produce "The capital of
  France is **Paris**."
* 9 build targets, zero warnings; `backend.c` still strict C89;
  `i486` still emits no MMX; all three `.exe` still 3 DLLs, no post-XP
  API.

---

## 1.4.0 — fewer FPU stalls

Two changes, both aimed at the finding from the 1.3.0 Geode profile:
the kernel was running at **~9 cycles per MMX instruction** where the
Geode's documented pipeline throughput is ~2. It was latency-bound, not
work-bound.

### 1. One `EMMS` per row instead of one per 64 weights

`EMMS` switches the FPU between MMX and x87 state. OLPC's Geode notes
warn that "every data exchange requires synchronization, which can
consume a LOT of cycles" — the LX's FPU is asynchronous to the integer
unit.

The 1.3.0 kernels issued an `EMMS` after every 64-weight sub-row, which
for this model works out at roughly **8 million `EMMS` per forward
pass**. Measured on the reference host, wrapping a small MMX block in
`EMMS` costs ~3.6x the block's own work.

Every per-format kernel is now split into two phases: all MMX blocks for
a whole matrix row run first, accumulating raw integer dot products into
a scratch array; then a *single* `mmx_sync()`; then all the float scale
arithmetic. That takes the count from ~8.05M to ~0.50M per pass — **16x
fewer FPU state switches**.

### 2. Dependency-friendly instruction scheduling (Q4_K)

The Q4_K chunk funnelled all four `pmaddwd` results through one scratch
register and accumulated two of them into the same register back to
back, so nearly every instruction waited on its predecessor.

Now the two `punpck` halves, the four masked vectors and the four
`pmaddwd` destinations are all distinct registers, and the four `paddd`
alternate between two accumulators. A microbenchmark of the serial
versus interleaved shape measured **1.97x** on a fully serial chain.

Q4_K throughput on the 32-bit build: **4071 → 5125 Mflop/s**.

Q5_K and Q6_K were left alone. Q5_K in particular needs two more live
temporaries than MMX's eight registers allow once two are accumulators;
forcing it would spill and lose more than it gains. If you want to push
further, that is where the remaining headroom is.

### Measured

32-bit `geode` build, same prompt, greedy:

| backend | 1.3.0 | 1.4.0 | gain |
|---|---|---|---|
| `mmx` | 10.4 s | 9.3 s | **1.11x** |

**Caveat, stated plainly:** this was measured on an out-of-order x86,
which hides exactly the stalls these changes remove. The real gain on
the in-order Geode is unknown from here and could be anywhere from
~1.1x to substantially more — the 1.3.0 kernels showed 1.72x on this
host and **2.08x** on the Geode, so the development host consistently
under-reports. Only the hardware can answer it.

### Verification

* **2,244 comparisons** — all 320 tensors x 6 activation patterns x both
  fast backends against the scalar reference. Zero failures, worst
  relative L2 1.219e-02, byte-identical to 1.3.0.
* All three backends still produce "The capital of France is
  **Paris**."
* 11 build targets, zero warnings; `i486` still emits no MMX; all three
  `.exe` still import only KERNEL32/msvcrt/WS2_32 with no post-XP API;
  PE `.rdata` still 8-byte aligned.

### Where the remaining time goes

From the 1.3.0 Geode profile, per forward pass (12.85 s):

| | share |
|---|---|
| feed-forward | 36.8% |
| delta-net input projections | 26.7% |
| lm head | 16.1% (11 calls, 5.5 s each) |
| delta-net output projection | 7.7% |
| attention | 6.1% |
| state recurrence | 5.1% |

At ~594 MMAC per token and 4 MACs per `PMADDWD`, pure instruction issue
at the documented 2-cycle throughput would be ~2.8 s per pass. We are at
12.85 s. The gap is stalls, which is why scheduling — not more
arithmetic — is the lever that remains.

Honest assessment of the 5 s/token target: it needs ~3.4x from here.
That is at the edge of what the hardware allows, and getting there would
require the Q5_K/Q6_K kernels rescheduled as carefully as Q4_K now is,
plus probably a smaller quantisation. Not impossible; not close to
guaranteed.

---

## 1.3.0 — faster MMX kernels, and a real alignment bug fixed

**Summary: `mmx` is ~1.7x faster end-to-end, `i8` ~1.1x, output
byte-identical.** The kernel rewrite came from an independent
contributor; this release integrates it after an audit that found and
fixed a latent crash.

### The bug that had to be fixed first

The contributed `backend_mmx.c` declared its MMX constants as bare
`short[4]`:

```c
static const short c_mask_f[4] = { 0x000F, 0x000F, 0x000F, 0x000F };
```

and loaded them with `movq`, **which requires an 8-byte-aligned source
operand**. A `short[4]` is only 2-byte aligned per the C standard, and
GCC gives `.rodata` 4-byte alignment on 32-bit x86. Verified against the
actual object file: **all five constants landed at addresses ≡ 4 (mod
8) — every one misaligned.**

An out-of-order x86 splits a misaligned MMX load transparently, so this
is completely invisible on a modern development machine. It is a
CPU-specific courtesy, not an architectural guarantee: it costs a
penalty on every access and can fault outright on strict, emulated or
virtualised implementations. It was reported as a hard failure on real
Geode LX and Windows XP hardware.

Note the contributor's report did state the disassembly had been audited
and was clean — and it was, for *instruction set*. Data alignment is a
separate axis that nobody checked.

**Fix:** an explicit `__attribute__((aligned(8)))` on each constant.

A `union` with `long long` is **not** sufficient — the i386 SysV ABI
aligns `long long` to 4, not 8, so GCC emits no `.align 8` and the
constants stay misaligned. That was tried first and rejected after
checking with `readelf`.

Measured effect on the emitted objects:

| | ELF `.rodata` | PE `.rdata` |
|---|---|---|
| before | align 4 — **broken** | `2**2` — **broken** |
| after  | align 8 — correct | `2**3` — correct |

`tests/t_align.c` now asserts at run time that all five constants are
8-byte aligned, so a compiler that ignores the attribute is caught by
`make test` instead of by a crash on the target. `backend_mmx.c` also
now `#error`s on non-GCC compilers rather than silently emitting
misaligned data.

### Kernel improvements (contributed, verified, kept)

* **In-register nibble unpacking.** Weights are expanded straight into
  MMX registers with `PUNPCKLBW`/`PUNPCKHBW` + `PAND`/`PSRLW` instead of
  a scalar loop writing to a staging buffer. The old code accelerated
  only the multiply and left the unpack — the more expensive half —
  scalar, plus 256 bytes per group of pointless store/reload traffic.
* **Real MMX Q6_K kernel.** Previously Q6_K fell back to scalar code
  entirely, and Q6_K is ~30% of matvec time (it carries the LM head).
  The −32 bias is folded into the weights with `PSUBW`.
* **MMX Q8_0 and Q4_0 kernels** (previously scalar).
* **Batched `EMMS`** — one per sub-row (Q4_K/Q5_K) or per half-block
  (Q6_K) instead of one per 32 lanes.
* **Precomputed per-16 activation sums** (`bk_qx.s16[]`) for the Q6_K
  bias term, instead of re-summing 16 elements eight times per block.
* **Branch-free Q5_K hi-bit**: `((h >> sh) & 1) << 4` rather than
  `(h & u) ? 16 : 0`.

The last two also speed up the portable `i8` backend, which needs no
MMX and still runs on a 486.

### Measured

32-bit `geode` build, 19-token prompt + 8 tokens, greedy:

| backend | 1.2.0 | 1.3.0 | speedup |
|---|---|---|---|
| `i8`  | 20.0 s | 17.9 s | **1.12x** |
| `mmx` | 18.0 s | 10.5 s | **1.72x** |

Per-kernel throughput, same build (Mflop/s):

| format | `ref` | `i8` | `mmx` |
|---|---|---|---|
| Q4_K | 1462 | 2085 | **4279** |
| Q5_K | 1306 | 1667 | **2847** |
| Q6_K | 335  | 2016 | **2734** |
| Q8_0 | 2264 | 1665 | **2451** |

### Verification

* **2,244 comparisons** — every one of the model's 320 tensors × 6
  activation patterns (uniform, large, tiny, sparse, alternating,
  all-zero) × both fast backends, each against the scalar reference.
  **Zero failures**; worst relative L2 error 1.2e-2, consistent with
  int8 activation quantisation and unchanged from 1.2.0.
* All six build/backend combinations produce byte-identical output.
* `i486` target still emits **zero** MMX instructions.
* `backend.c` still compiles clean under `-std=c89 -pedantic -Wall
  -Wextra`.
* All three `.exe` still import only KERNEL32, msvcrt and WS2_32, with
  no post-XP API.

### Rejected

The contributed report recommended adding 3DNow! `PREFETCH` on the
grounds that the workload is memory-bound. **Profiling on real Geode
hardware shows it is not:** 14 MB/s of ~390 MB/s available (3.6%
utilisation) and 22.5 cycles per MAC. The workload is compute-bound and
prefetching would gain approximately nothing. That recommendation
originated from an outdated note in this project's own README, since
corrected.

### Other

* Version string bumped to 1.3.0 (the profiling logs in 1.2.0 still
  reported 1.0.0).
* `make test` now includes `t_align`.

---

## 1.2.0 — profiling

* Compile-time stage profiling (`-DINFER_PROFILE`, `make profile*`),
  zero overhead when not enabled.
* Always-available throughput logging (`--log-perf`): TTFT, prompt and
  generation tokens/second.
* `--log-file` to write either to a file.
* High-resolution monotonic clock in the platform layer
  (`gettimeofday` / `QueryPerformanceCounter`).

## 1.1.0 — selectable compute backends

* `ref` (scalar float, 486-safe), `i8` (integer, 486-safe), `mmx`.
* Run-time selection via `--backend`, CPUID probing, `--backend list`.
* `make geode`, `make windows-mmx`.

## 1.0.0 — initial release

* GGUF loader, `qwen35` hybrid engine, byte-level BPE tokeniser,
  OpenAI-compatible HTTP server.
* ANSI C (C89), no dependencies, POSIX and Windows XP backends.
