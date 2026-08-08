# Performance

Real numbers, the hard ceilings, and — most importantly — **how to
optimise this codebase without fooling yourself**.

- [The optimisation loop](#the-optimisation-loop)
- [Current numbers](#current-numbers)
- [The memory ceiling](#the-memory-ceiling)
- [Where the time goes](#where-the-time-goes)
- [Threading](#threading)
- [Memory](#memory)
- [What is left](#what-is-left)
- [Rejected optimisations](#rejected-optimisations)

---

## The optimisation loop

This project has produced roughly a 4x speed-up and also a long list
of confident, plausible, **wrong** ideas. The difference was always
method, never cleverness.

```
        ┌──────────────────────────────────────────────────┐
        │                                                  │
        ▼                                                  │
 1. TEST AND RECORD                                        │
        │                                                  │
        ▼                                                  │
 2. SEARCH FOR MORE TESTS  ── what else can I measure?      │
        │                                                  │
        ▼                                                  │
 3. RUN THOSE TESTS        ── now you know WHERE it hurts   │
        │                                                  │
        ▼                                                  │
 4. SEARCH FOR SOLUTIONS   ── how have others fixed THIS?   │
        │                                                  │
        ▼                                                  │
 5. IMPLEMENT                                              │
        │                                                  │
        ▼                                                  │
 6. TEST AGAIN             ── full model, interleaved       │
        │                                                  │
        ├── better? keep ──────────────────────────────────┤
        └── not better? 7. REVERT ─────────────────────────┘
                                                           │
                        repeat until the goal is reached ──┘
```

**The two search steps are different and the order matters.** Step 2
searches for *diagnostics* — you do not yet know what is slow, so you
are looking for ways to find out. Step 4 searches for *fixes*, and
only makes sense once step 3 has told you what to fix. Searching for
solutions first is how you end up implementing a cache-locality fix
for a problem that turned out to be a scale decode.

### 1. Test and record

Establish a baseline **and write it down**. An unrecorded number is a
number you will re-measure under different conditions and misread.

```sh
infer run model.gguf -p "..." -n 40 -t 0 --log-perf     # end to end
infer run model.gguf -p "..." -n 40 -t 0 --log-stages   # per stage
```

Record: platform, binary, thread count, token count, and the **median
of several runs** with its spread. `uptime` first — the machine must
be idle.

Keep the log across the whole session. Half the value of this loop is
being able to compare iteration 6 against iteration 1 honestly.

### 2. Search for more tests

You have one profile. That is rarely enough to locate a bottleneck,
and often the profile itself is lying to you about units. So go and
find **what else can be measured**, before deciding what is wrong.

Worth searching for, and all of which paid off here:

- how others benchmark this workload (`llama-bench`, `perf stat`,
  cachegrind, `getrusage`, `/proc/<pid>/status`)
- what the theoretical ceiling is — a **streaming-bandwidth
  microbenchmark** turned the whole AVX2 question from "is the kernel
  slow?" into "we are at 67% of a hard 21.7 tok/s ceiling"
- what counters exist for the specific suspicion — context switches
  and CPU% exposed the thread-pool barrier (159% CPU on two cores)
  when tok/s alone just looked disappointing
- whether the platform has a known measurement trap — Wine's
  `SetEvent` costs 35 us against ~1 us on real Windows, which
  invalidates any conclusion drawn from it

This step is what produced the two most important measurements in the
project: the **11.3 GB/s streaming ceiling**, and the
**time-by-subtraction** breakdown below.

### 3. Run those tests

Now measure with the new instruments. Two techniques that repeatedly
found things nothing else did:

**Time by subtraction.** Strip the kernel to nothing and add one layer
at a time:

```
loads only                     0.015 ns/weight
+ nibble split                 0.016
+ VPMADDUBSW / VPMADDWD        0.019   <- all the real arithmetic
full kernel                    0.070
```

That says immediately that **73% of the kernel is neither loads nor
arithmetic** — it was the 6-bit scale decode. Reading the disassembly
had pointed at `VPBROADCASTW`, worth 7%.

**Normalise by work.** `us/call` is the wrong unit when call sizes
differ by 6x. A profile once said Q6_K cost 5.16x a Q4_K call and
drove an entire release; it was measuring tensor shape.

### 4. Search for solutions

*Now* you know what to search for. Read what the fast implementations
actually do rather than inventing it:

- `llama.cpp` `ggml/src/ggml-cpu/arch/x86/quants.c` — the kernels.
  Three separate wins came straight out of this file: the Q8_K
  activation plane, integer-domain scaling, and the `utmp`
  scale-decode trick that fixed the 73% above
- `ggml/src/ggml-common.h` — block layouts
- `src/models/qwen35.cpp` — the reference for this architecture
- vendor manuals for instruction cost and availability; QEMU's
  `vis_helper.c` for exact VIS semantics

**Check [`reference/findings.md`](reference/findings.md) first.** Your
idea may already have been measured and rejected here.

### 5. Implement

Smallest change that tests the idea. Do not bundle three
optimisations — when the result is ambiguous you will not know which
one did it. (The DeltaNet fusion looked like a 5% cache win until it
was separated from the FMA and unrolling that came with it; fusion
alone was ~1%.)

### 6. Test again — in the full model

**Isolated kernel wins do not always survive.** A Q5_K change improved
the kernel 0.103 → 0.086 ns/weight and made the model *slower*
(12.9 vs 13.3 tok/s). A VIS bias trick improved the kernel and cost
1.4% end to end.

```sh
for r in 1 2 3 4 5; do
  for build in A B; do ... -n 40 -t 0 --log-perf ; done
done
```

**Interleave the builds** — never all of A then all of B; machine
state drifts. Medians, with the spread. Re-check `uptime`: a runaway
process once sat at 97% CPU for 25 minutes and halved every number
taken in that window.

Also re-run correctness: `t_ident`, `t_thread`, and bit-identity
across thread counts if you touched anything parallel.

### 7. Revert when needed

**A change with no measured benefit does not ship, however elegant.**
Recent examples: the Win32 conditional `SetEvent` (no gain, plus a
suspected liveness bug) and the DeltaNet fusion (~3%, cost
bit-identity).

Then **record the negative result** in
[`../docs/FINDINGS.md`](../docs/FINDINGS.md) so nobody — including
you, in three weeks — retries it.

### 8. Repeat until the goal is reached

Have an explicit target and a stopping condition. "Faster" is not a
goal; "≥15 tok/s single core" is. Equally important is knowing when to
declare a path **finished**: the MMX backend is instruction-bound at
1.19 instructions per weight with no unsigned-byte MAC available, so
perfect kernels top out around 1.37x. That path is closed, and saying
so is more useful than another round of micro-tuning.

### Expect to be wrong

Most hypotheses on this project were, including several that were
obvious:

| hypothesis | result |
|---|---|
| transparent hugepages will help | **wrong** — file mmap measured *faster* than anon+MADV_HUGEPAGE (12.58 vs 10.91 GB/s) |
| the 18.9 MB DeltaNet state evicts weights from L3 | **wrong** — touching 1 MB between matvecs made it *faster*; noise |
| prefetch overshooting the row end wastes bandwidth | **wrong** — overshoot helps; rows are contiguous |
| the Win32 `SetEvent` storm costs real time | **wrong under Wine** — no measurable gain, and the patch introduced a suspected liveness bug. Reverted |
| the scale decode is cheap next to the arithmetic | **wrong** — it was 73% of the kernel |
| threading is not worth it, the bus is saturated | **wrong** — the barrier was the bottleneck, not the bus (+28% once fixed) |

This is why the loop measures before searching for fixes, and measures
again after implementing them.

## Current numbers

Sandbox Xeon @2.60 GHz, 2 cores, `Qwen3.5-0.8B-Q4_K_M`, 40 tokens
greedy, idle machine, **Linux 64-bit**.

### By backend, single thread

| backend | tok/s | bound by |
|---|---|---|
| `avx2` | 12.5 | memory |
| `i8` | 3.27 | instructions |
| `mmx` | 3.26 | instructions |
| `ref` | 0.99 | reference only |

### By thread count, avx2

| threads | tok/s |
|---|---|
| `-T 1` | 12.8 – 14.5 |
| `-T 2` | 18 – 22 |

The spread is real: this is a shared 2-core VM and run-to-run
variation is ±10%. Quote medians.

### Other platforms

| platform | `-T 1` | `-T 2` | note |
|---|---|---|---|
| Linux 64-bit | 12.8 | 18.2 | native |
| Windows 64-bit under Wine | 10.8 | 12.6 | Wine adds ~16% flat overhead |
| Linux 32-bit | ~10.7 | | half the registers |
| i7-9750H, real Windows | ~12 | ~21 | user hardware |
| Geode LX 800 @500 MHz | ~0.06 | | ~15.8 s/token, `mmx` |

**The Wine figures are not Windows figures.** At `-T 1` the pool is
never entered and the kernels are byte-identical code, so the 16% gap
at one thread is entirely Wine's libc/syscall layer. Wine also
emulates the synchronisation primitives badly — `SetEvent` measures
~35 us under Wine against ~1 us on real Windows — so the scaling
deficit it shows is an *upper bound*, not a real Windows result.

## The memory ceiling

This workload is **memory-bound**, and the ceiling is arithmetic you
can do on paper:

- The model moves **~520 MB per generated token** (311 MB of layer
  weights + 208.6 MB of LM head).
- Measured single-core streaming read on this machine: **11.3 GB/s**.
- Therefore the hard single-core ceiling is
  `11.3e9 / 520e6` = **21.7 tok/s**.

At 14.5 tok/s we use 7.5 GB/s, i.e. **67% of the ceiling**. Two
threads reach 20.9 GB/s aggregate, which is why `-T 2` scales nearly
linearly while further kernel work does not.

Prefetch and cache experiments will not move this. **Only moving fewer
bytes will** — see [What is left](#what-is-left).

## Where the time goes

`--log-stages`, 58 forward passes, single thread:

| stage | share |
|---|---|
| feed-forward (24 layers) | 33.8% |
| delta-net layers (18) | 32.4% |
| — of which input projections | 19.6% |
| — state recurrence | 4.7% |
| — output projection | 5.9% |
| **lm head** | **27.4%** |
| attention layers (6) | 6.3% |
| rms norms | 0.2% |

By weight format (overlapping the above): Q4_K 34.5%, Q6_K 34.9%,
Q5_K 20.5%, Q8_0 0.4%.

**~81% of a token is matvec.** That is where any remaining gap to
`llama.cpp` lives, not in the recurrence (4.7%) or the norms (0.2%).

## Threading

Until 1.23.0 the pool's own barrier was the bottleneck once the
kernels got fast:

| | before | after |
|---|---|---|
| one `tp_parallel_for` barrier | 44–48 us | **0.25 us** |
| `nrows=64` vs serial | 0.39x (slower!) | 1.93x |
| `nrows=4096` vs serial | 1.87x | 2.01x |
| model, `-T 2` | 17.25 tok/s | **22.16 tok/s** |
| CPU utilisation, 2 cores | 159% | 181% |
| voluntary context switches | 17,402 | 4,510 |

A forward pass issues ~95 parallel regions, so a 45 us barrier is
milliseconds per token of pure sleep/wake.

**The general lesson:** once the work per parallel region shrinks, the
synchronisation guarding it has to shrink too. And a fast path that is
only fast under an assumption must *detect* when the assumption fails
and fall back to the code it replaced — spinning unconditionally cost
`-T 4` on 2 cores 17.2 → 10.4 tok/s.

## Memory

- Idle RSS is **~44 MB** (42 MB anonymous: KV cache, DeltaNet state,
  scratch). That is before any token is generated.
- Generating touches every weight, so peak RSS reaches **~560 MB** for
  the 508 MiB model. Those pages are file-backed and clean, so the
  kernel evicts rather than swaps — the model runs on a machine with
  less RAM than the file, it just pages.
- Anonymous memory stays ~42 MB regardless of context length in 18 of
  24 layers, because DeltaNet holds a fixed-size state.

## SPARC / UltraSPARC

A different machine with a different bottleneck, worth stating
separately because the x86 conclusions do not transfer.

Reported from real hardware (UltraSPARC, `vis` backend, 0.8B Q4_K_M):

```
prompt      : 17 tok in 3.4 min   (11.91 s/tok)
generation  : 10 tok in 4.3 min   (25.88 s/tok)
```

A forward pass is **739 M weights = 1.48 Gflop**, so 25.88 s/tok is
**~57 Mflop/s**. The isolated `--kernel bench` on the same machine
gives 39–56 Mflop/s, so **generation runs at kernel speed** — the CPU,
not the disk, sets the generation rate.

Prompt tokens are 2.2x faster than generation tokens because the LM
head (254 M weights, 34% of the pass) runs once per *generated* token
and is skipped for all but the last prompt token.

### Where the time goes

| format | share of a token | VIS kernel? |
|---|---:|---|
| Q4_K | 34.5% | yes |
| Q5_K | 20.5% | yes |
| Q6_K | 34.9% | **no — falls back to `i8`** |
| Q8_0 | 0.4% | no |

**35% of every SPARC token runs on the portable path**, and it is the
LM head.

### The three things that were wrong

1. **`MADV_SEQUENTIAL` on Solaris means "free pages behind me".** On
   Linux it only widens readahead. The access pattern restarts every
   token, so on Solaris the hint discarded exactly the pages the next
   token needed — matching the reported "VERY high disk usage". Now
   `MADV_WILLNEED` only. (Finding 56.)
2. **`-O3` spills on a register-poor machine.** VIS operands live in
   the FP register file and SPARC has no int↔FP move, so every
   integer-side value costs a store+load. `-O2 -funroll-loops`
   measured 16.0 insns and 3.7 spills per `fmul8x16` against `-O3`'s
   19.5 and 4.2. (Finding 57.)
3. **`sum16()` — a one-line array read — was eight real function calls
   per Q6_K super-block.** Force-inlining it and `rd_f16p()` took
   `i8_dot_q6_K` from 328 to 272 instructions and halved its spills.
   (Finding 58.)

### What did not work

**Enabling the VIS Q6_K kernel.** It is 440 instructions per
super-block against the portable path's 328 — 1.34x *more*. A Q6_K
weight is split across two planes, so reassembling it costs
mask+shift+or per weight on the integer side, and every value then has
to cross the int→FP gap. The format defeats the instruction. Finding
30 was right; it stays compiled out. (Finding 59.)

### Confirmed on real hardware (1.24.0)

The user's UltraSPARC, before and after:

| | 1.23.x | 1.24.0 |
|---|---:|---:|
| generation | 25.88 s/tok | **13.87 s/tok** |
| prompt | 11.91 s/tok | **7.55 s/tok** |
| matvec throughput | ~57 Mflop/s | **126.0 Mflop/s** |

**1.87x on generation**, beating the 15 s/tok minimum. I predicted
~10% from the instruction-count work and said the `madvise` fix could
not be sized from the sandbox. It was the dominant one: the disk was
taking roughly half the machine.

The remaining profile is now honest compute — Q6_K is 45.3% of matvec
time, and it still runs on the portable path.

### Does any of this transfer to x86?

**No, and the reason is worth knowing.** Checked all three parts:

- **The `madvise` bug is Solaris-only.** Windows maps with
  `CreateFileMapping`/`MapViewOfFile`, and per Microsoft
  "memory-mapped file access does not go through the cache manager, so
  these cache hints have no effect" — the `FILE_FLAG_RANDOM_ACCESS` on
  the handle never touches the hot path. Linux never freed behind us.
- **The inlining win is real in the object code**: the shipped Windows
  32-bit build goes 200 → 186 share-weighted instructions (−7%), with
  Q6_K dropping 330 → 289 and 9 calls → 1.
- **And it is worth 0% on the clock.** Measured, 32-bit, three
  interleaved pairs: `i8` 1.418/1.420/1.379 → 1.400/1.421/1.405;
  `mmx` 3.099/3.101/3.046 → 3.041/3.057/3.050.

The saving is eight *function calls* per super-block. On SPARC a call
rotates a register window and can trap to spill sixteen registers, on
an in-order single-issue core that cannot hide it. On x86 a call is a
push and a jump. Same instruction saving, different cost model,
different outcome. (Finding 60.)

### Honest expectation

Superseded by the measurement above: the real machine gave 1.87x, not
the ~10% the instruction counts alone predicted. Recorded because the
prediction was wrong in an instructive direction — the effect that
could not be measured in the sandbox was the one that mattered.

Remaining headroom on SPARC: Q6_K is 45.3% of matvec time and runs on
the portable path, and the VIS kernel for it is measurably worse
(finding 59). Closing the last gap to 10 s/tok needs either a better
Q6_K approach or fewer bytes (see below).

## What is left

Nothing in the kernels. What remains changes the workload:

1. **A smaller model or quantisation.** The only lever with a multiple
   in it. `Q4_K_S` cuts ~10% of the bytes; a 0.4B-class model roughly
   halves everything.
2. **Batched prompt ingestion.** Untouched. Every prompt token
   re-unpacks the whole model, so this affects time-to-first-token
   only.
3. **Requantising the LM head** (`output.weight`) from Q6_K to Q4_K.
   It is 27.4% of the pass and **208.6 MB** — 39% of the file, 27.6%
   of bytes per token. Q4_K would make it 143.0 MB, saving 65.6 MB per
   pass. It changes the output distribution, so it is a different
   model, not an optimisation.
4. **Vocabulary pruning.** 248,320 tokens is most of why the LM head
   is huge. Cutting to ~32k takes ~8x off it, but it is
   deployment-specific.

Items 3 and 4 are the only routes to the MMX target (4.5–7 tok/s) that
kernel work cannot reach: perfect MMX kernels top out around 1.37x of
today, i.e. ~4.35 tok/s.

### Why MMX is at its floor

`Q4_K` runs at **1.19 MMX instructions per weight**, and six of its
nineteen instructions per sixteen weights are `PUNPCK` widening that
exists only because `PMADDWD` consumes words. **MMX has no
unsigned-byte multiply-accumulate** — `PMADDUBSW` is SSSE3 (2006), and
Extended MMX on the Geode adds only `PSHUFW`/`PMAXSW`/`PAVGB`/
`PSADBW`/`MOVNTQ`/`PREFETCH`, none of which help. MMX uses only ~14%
of available memory bandwidth: it is instruction-bound, not
memory-bound, which is the opposite of AVX2.

## Rejected optimisations

Recorded so they are not retried. Each was measured.

| idea | verdict |
|---|---|
| transparent hugepages | file mmap already faster than anon+HUGEPAGE |
| L3 eviction by DeltaNet state | not happening; touching memory between matvecs made it faster |
| shorter prefetch distance to avoid overshoot | overshoot helps; 32 is the tuned value |
| 4-row matvec kernels | worse than 2-row (register pressure) |
| Q5_K split-loop variant | kernel 17% faster, model 3% slower |
| VIS Q6_K kernel | 3.1x fewer instructions, no end-to-end change. Compiled out by default |
| VIS bias trick | kernel 0.420 → 0.409 ns/w, model 3.173 → 3.129. Reverted |
| MMX reduction-rate halving | 0.601 → 0.598 ns/w. Nothing |
| DeltaNet two-pass → fused | ~3%, but see below |
| Win32 conditional `SetEvent` | no gain under Wine, suspected liveness bug. Reverted |

The DeltaNet fusion is worth a note because the *reasoning* offered
for it was wrong even though the result was mildly positive: the
proposal argued from DRAM traffic and L1 capacity, but each head's
64 KB state is L2-resident (measured 49.55 GB/s across 16 heads, far
above the 11.3 GB/s DRAM ceiling). The win that survived was algebraic
— hoisting `<k,q>` out of the row loop — not a cache fix. At 4.4% of a
token it was not worth the bit-identity loss.
