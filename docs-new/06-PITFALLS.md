# Pitfalls

Every trap that has cost real time on this project, with the symptom
first so you can search for what you are seeing.

Long-form write-ups with full measurements are in
[`../docs/FINDINGS.md`](../docs/FINDINGS.md); the number in the last
column is the finding to read.

- [Quick lookup by symptom](#quick-lookup-by-symptom)
- [Threading](#threading)
- [Portability](#portability)
- [Build system](#build-system)
- [Kernels and numerics](#kernels-and-numerics)
- [Measurement](#measurement)
- [Tooling and workspace](#tooling-and-workspace)
- [Process](#process)

---

## Quick lookup by symptom

| you are seeing | cause | § |
|---|---|---|
| fluent but wrong output, only when threaded | `static` scratch in a kernel | [T1](#t1) |
| Windows produces nothing above `-T 1` | bare `if` around a Win32 event wait | [T2](#t2) |
| `-T 4` slower than `-T 2` on 2 cores | unconditional spinning | [T3](#t3) |
| threads barely help at all | barrier costs more than the work | [T4](#t4) |
| `!!!!!!!!` after minutes on SPARC | byte order guessed wrong | [P1](#p1) |
| fp16 decodes to zero on 64-bit big-endian | type width, not byte order | [P2](#p2) |
| faults on a Geode, fine on your desktop | MMX constants misaligned | [P3](#p3) |
| illegal instruction on an old CPU | non-baseline flag leaked out of the gated object | [P4](#p4) |
| Solaris build dies on `off_t` | 32-bit `-Xc` makes it a union | [P5](#p5) |
| Studio: "`-W` option with unknown program all" | `CC ?= cc` cannot override make's default | [B1](#b1) |
| `bits/libc-header-start.h: No such file` | missing `gcc-multilib` | [B2](#b2) |
| four DLLs, one is `libwinpthread-1.dll` | posix-threads mingw | [B3](#b3) |
| `multiple definition of bk_a2_amax` | missing `-DINFER_A2_LINKED` | [B4](#b4) |
| `-T 4` reports 1 thread | built without `-DINFER_HAVE_THREADS` | [B5](#b5) |
| AVX2 backend silently missing | tested `__AVX2__` instead of `INFER_HAVE_AVX2` | [B6](#b6) |
| output diverges from other backends ~token 8 | FMA contraction | [K1](#k1) |
| `avx2` differs from `i8` on k-quants | by design; bounded by `t_avx2acc` | [K2](#k2) |
| tool call parsed "successfully" with no arguments | only one of two shapes handled | [K3](#k3) |
| a kernel win vanishes in the model | microbenchmark ≠ model | [M1](#m1) |
| numbers halved for no reason | a runaway process is eating a core | [M2](#m2) |
| Q6_K "is 5x the cost of Q4_K" | `us/call` compares shapes, not kernels | [M3](#m3) |
| workspace full, model evicted | Wine prefix is 1.4 GB | [W1](#w1) |
| `wine: '/tmp' is not owned by you` | prefix must be a directory you own | [W2](#w2) |
| CI green but nothing ran | GitHub Actions outage / dropped webhook | [W3](#w3) |

---

## Threading

### T1 — `static` scratch in a kernel {#t1}

**Symptom:** fluent, plausible, *wrong* output — only with `-T 2` or
higher. Every single-threaded test passes.

**Cause:** the MMX kernels held a per-row accumulator in
`static int acc[]`. Under threading every worker wrote the same array.

**Avoid:** kernel scratch is **automatic** (stack), never `static`.
CI greps the backend sources for `static` arrays and fails on any.

**Why four test rounds missed it:** `t_ident` was single-threaded; the
shape sweep was single-threaded; an instrumented cross-check
serialised the pair and hid it; and `t_thread` went through
`q_matvec` → `auto` → `avx2`, so **MMX was never threaded by any
test**. `t_thread` now loops over every backend **by name**.
*(Finding 52)*

### T2 — a bare `if` around an OS wait {#t2}

**Symptom:** Windows generates nothing at `-T 2`+; `-T 1` is fine.
Actually a crash with a register dump, not a clean exit.

**Cause:** the Win32 worker used `if (tp_sgen == mygen) WaitFor…`
while the pthread worker used `while`. `tp_go` is an **auto-reset
event** signalled every batch, so a signal latched during spinning
made the next wait return with no new work. The worker re-ran the
previous slice and stamped a stale generation.

**Avoid:** every blocking wait re-checks its predicate in a **loop**,
on every platform. CI asserts the source shape because no Linux test
can observe it. *(Finding 55)*

### T3 — spinning when oversubscribed {#t3}

**Symptom:** `-T 4` on 2 cores is much slower than `-T 2`
(17.2 → 10.4 tok/s).

**Cause:** spinners steal the CPU from the workers they are waiting
on.

**Avoid:** the spin budget is derived from `tp_cpu_count()` and set to
zero when oversubscribed, at which point **both** sides take the
original blocking path — the old code, not a degraded imitation.
`sched_yield()` in the spin loop was tried and was still a regression.
*(Finding 54)*

### T4 — a barrier that costs more than the work {#t4}

**Symptom:** threading buys almost nothing; CPU sits at 159% on two
cores; huge voluntary-context-switch count.

**Cause:** ~95 parallel regions per token, each a 44–48 us
condition-variable round trip. Shapes under ~1024 rows were *slower*
threaded than serial.

**Avoid:** spin briefly before blocking (0.25 us barrier). More
generally: when kernels get faster, re-measure the synchronisation
that guards them. *(Finding 54)*

---

## Portability

### P1 — byte order guessed wrong {#p1}

**Symptom:** `!!!!!!!!!!` after eight minutes on an UltraSPARC.

**Cause:** the `#if` tested `__sparc__` and `__BYTE_ORDER__`, which
are **GCC** spellings. Sun Studio defines `__sparc` (one underscore).

**Avoid:** test every vendor spelling, and make the `#else` an
`#error`. **A portability macro that guesses converts a build error
into a wrong answer.** *(Findings 20, 27)*

### P2 — the endianness bug that was a type-width bug {#p2}

**Symptom:** fp16 decodes to zero, but only on 64-bit big-endian.

**Cause:** `q_fp16_to_fp32()` — which a source audit had explicitly
*cleared* as endianness-neutral. The three sites the audit predicted
were also broken, but this was the actual defect.

**Avoid:** do not trust a source audit over a run on the target.
*(Finding 18)*

### P3 — MMX constants misaligned {#p3}

**Symptom:** fine on x86, faults on a Geode.

**Cause:** constants declared `short[4]` get 4-byte alignment; `movq`
needs 8. A `union` with `long long` is **not** sufficient — the i386
SysV ABI aligns `long long` to 4.

**Avoid:** explicit `__attribute__((aligned(8)))`, asserted at run
time by `tests/t_align.c`. *(Finding 4)*

### P4 — a non-baseline flag leaks out of the gated object {#p4}

**Symptom:** illegal instruction before `main()` on an old CPU.

**Avoid:** only `backend_avx2.c` is compiled with `-mavx2`. CI
disassembles every other object and requires **0 CMOV, 0 SSE/AVX**.
*(Findings 34, 50)*

### P5 — `off_t` becomes a union on Solaris {#p5}

**Symptom:** the same error moves one line down after each "fix".

**Cause:** `-Xc` undefines `_LONGLONG_TYPE`, and in 32-bit `off_t`
becomes a union you cannot assign to.

**Avoid:** build 64-bit; in LP64 `off_t` is a plain `long`. **When two
fixes in a row move the error by one line, the premise is wrong.**
*(Finding 2 in PITFALLS lineage / Solaris section)*

### P6 — never name a 64-bit integer type

`long long` and `int64_t` do not exist in C89 and broke the Solaris
build **twice**. Use `long`, or a pair of 32-bit values. CI greps for
it.

---

## Build system

### B1 — `CC ?= cc` cannot override make's built-in {#b1}

Make assigns `CC = cc` **before reading the Makefile**, so `?=` never
fires. On Linux `cc` is GCC and the bug is invisible; on Solaris `cc`
is Studio and the GCC targets silently called the wrong compiler.

**Avoid:** a separate variable (`GNUCC`) for the GCC-only targets.
*(Finding 26)*

### B2 — missing 32-bit headers {#b2}

`make linux` is a **32-bit** build even on x86-64. Install
`gcc-multilib`.

### B3 — the wrong mingw variant {#b3}

The posix-threads mingw links `libwinpthread-1.dll` and breaks the
three-DLL guarantee. Install the `-win32` variant, and check the
**binary**, not the compiler banner:
`objdump -p x.exe | grep -ci libwinpthread` must be 0.

### B4 — the two-stage AVX2 build, three ways to break it {#b4}

| omission | symptom |
|---|---|
| `-DINFER_A2_LINKED` | `multiple definition of bk_a2_amax` |
| the object itself | `undefined reference to bk_a2_amax` |
| `-DINFER_HAVE_THREADS` | links fine, silently single-threaded |

### B5 — silently single-threaded {#b5}

If `-T 4` reports 1 thread, the build lacks `-DINFER_HAVE_THREADS`.
The pool compiles to stubs and nothing warns you.

### B6 — `__AVX2__` is false where you need it {#b6}

`backend.c` is compiled at the i486 baseline, so `__AVX2__` is false
there even when the AVX2 object is linked. Register the backend on
**`INFER_HAVE_AVX2`**. Testing the codegen macro silently drops the
kernel from every artifact. *(Finding 50)*

### B7 — the Makefile has no conditionals

Solaris `/usr/ccs/bin/make` has no `ifeq`. Every switch uses indirect
expansion `$(VAR_$(FLAG))`. Do not "tidy" this.

### B8 — a flag that does nothing is worse than no flag

`NO_SSE2=1` existed for several releases and produced a
**byte-identical binary** — no source read `INFER_HAVE_SSE2`. Removed
in 1.23.3. Verify each flag changes the output (`md5sum`).

---

## Kernels and numerics

### K1 — FMA contraction changes the answer {#k1}

**Symptom:** the AVX2 build diverges from the others at around token
8.

**Cause:** `-march=haswell` fuses `a*b+c` into `vfmadd` (27 sites in
`quant.c`), which has different rounding.

**Avoid:** `-ffp-contract=off`. Costs 2.8%. CI asserts it.
*(Finding 44)*

### K2 — `avx2` is deliberately not bit-identical {#k2}

On k-quants it quantises activations per 256 instead of per 32 and
reassociates. `Q8_0` matches exactly; `Q4_K`/`Q5_K`/`Q6_K` do not.
This is correct and intended. `t_ident` excludes it; `t_avx2acc`
bounds its error against float `ref`. **Do not "fix" `t_ident`.**

### K3 — a parser that partially matches {#k3}

**Symptom:** `tool call: add {"a":17}` — the `b` silently missing.

**Cause:** the model emits tool calls in two shapes; only the XML form
was handled, and the JSON form parsed to an empty argument object.

**Avoid:** a parser that can partially match must **say so** rather
than returning success. `t_agent` checks arguments, not just names.
*(Finding 16)*

### K4 — lane budgets must be checked against the loop

VIS and AVX2 accumulate in 16-bit lanes. Q6_K sits at 16002/32767
(AVX2) and 32004/32767 (VIS) — a 0.8% margin in the VIS case. Those
budgets are **tested exhaustively** (`t_avx2sat`, `t_vissat`), not
asserted in a comment. *(Finding 29)*

---

## Measurement

### M1 — a kernel win that does not survive the model {#m1}

A Q5_K change improved the kernel 0.103 → 0.086 ns/weight and made the
model **slower** (12.9 vs 13.3 tok/s). A VIS bias trick improved the
kernel and cost 1.4% end to end.

**Avoid:** always A/B the full model, interleaved, medians of 5+.
*(Findings 28, 31)*

### M2 — a runaway process eating a core {#m2}

An aborted Wine benchmark left a process at 97% CPU for 25 minutes.
Every measurement in that window was silently halved — including one
that made threading look like a regression.

**Avoid:** `uptime` before trusting a number;
`pkill -9 wineserver` between Wine runs.

### M3 — `us/call` compares shapes, not kernels {#m3}

A profile grouped by weight format said Q6_K was 5.16x a Q4_K call.
That drove a whole release and was measuring tensor size.

**Avoid:** normalise by elements (ns/weight) before comparing formats.
*(Finding 30)*

### M4 — microbenchmarks lie about small machines

On the Geode, microbenchmarks were systematically optimistic: the fast
L1 window is 4 KB, not 64 KB, and the machine is compute-bound where
the desktop is memory-bound. *(Findings 6, 7, 10)*

### M5 — comparing two platforms as if they were one

"22 tok/s" was a **Linux** number quoted without its platform, then
compared against a Windows run and read as a regression. Always state
the platform, the thread count and the binary.

---

## Tooling and workspace

### W1 — Wine's prefix is 1.4 GB {#w1}

It will eat the workspace budget and get large files (like the model)
evicted mid-run.

**Avoid:** `export WINEPREFIX="$HOME/.cache/wineprefix"` — `.cache` is
excluded from workspace snapshots. Delete it when finished:
`rm -rf ~/.cache/wineprefix`.

### W2 — `WINEPREFIX` must be a directory you own {#w2}

`/tmp` fails with `wine: '/tmp' is not owned by you, refusing to
create a configuration directory there`.

### W3 — CI can be green because it never ran {#w3}

**Symptom:** you push a tag, nothing appears in Actions. Not a failed
run — **zero check suites**.

**Causes seen:** a GitHub Actions outage throttling webhooks (only
~15% delivered); and the documented rule that **pushing more than
three tags at once creates no events at all**.

**Avoid:** push tags one at a time (`git push origin v1.2.3`, never
`--tags` with a backlog). Check <https://githubstatus.com> before
debugging your workflow. Watch the Actions tab after a tag push rather
than assuming.

Every guard in CI assumes a run *happens*: if Actions silently stops,
the version guard, changelog guard and doc guard all "pass" by never
executing.

### W4 — delete the model when you finish

It is 508 MiB and the workspace prunes large files. Re-download costs
~15 s; a surprise eviction mid-benchmark costs a lot more.

```sh
rm -f ~/models/*.gguf
```

---

## Process

### Pr1 — a probe must be as hard as the build it gates

A capability probe that compiles `int main(void){return 0;}` needs no
kernel headers, so it links happily while `asm/errno.h` is missing —
then `make all` dies four steps later. The probe compiles the **real
headers** the project uses. *(Findings 43, 45)*

### Pr2 — `apt-get install` is atomic

If one package name cannot be resolved, apt exits 100 and installs
**nothing**. `|| echo warning` turned "no compilers at all" into a
green step. Split required and optional packages into two commands.
*(Finding 42)*

### Pr3 — `git add -A`, never `git add *`

The shell glob skips dotfiles and once missed the entire `.github/`
directory.

### Pr4 — root-cause, do not patch symptoms

A run of CI fixes that each patch one symptom is a signal the model of
the problem is wrong. Stop and find the actual cause.

### Pr5 — a platform you cannot execute is a platform you cannot claim

Four releases of "verified" Windows binaries rested on static
inspection of imports and opcode counts. None of that can see a logic
error in a wait loop. **Wine works** — the assumption that it did not
was based on one misdiagnosed failure.

### Pr6 — negative-control every guard

A guard that has never failed is a guard you have not tested.
Deliberately break the property and confirm the guard fires. Two
guards written for this project had false positives found exactly this
way.
