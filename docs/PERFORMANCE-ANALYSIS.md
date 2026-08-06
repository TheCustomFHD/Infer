# Where the time goes, and why the kernels are done

A record of the optimisation effort on `Qwen3.5-0.8B-Q4_K_M`, so the
reasoning is not lost and the dead ends are not retried.

Most of this document concerns the **AMD Geode LX 800 @ 500 MHz** and
the MMX path. Figures are from that hardware unless stated; the AVX2
section below is measured on a Xeon @ 2.60 GHz.

---

## The short version

**Geode LX 800, MMX path:**

| backend | s/token | cycles/MAC | status |
|---|---|---|---|
| `mmx` | 15.80 | 8.9 | **at its floor** |
| `i8` | 33.29 | 21.8 | **at its floor** |
| `ref` | ~90 | — | reference only |

**Modern x86, single core, generation tok/s (Xeon @ 2.60 GHz):**

| backend | tok/s | bound by |
|---|---|---|
| `avx2` | 14.5 | memory — ~71% of the streaming ceiling |
| `mmx` | 3.2 | instructions — only ~14% of bandwidth used |
| `i8` (64-bit, vectorised) | ~12.8 | memory |

The two paths are limited by different things, which is why they need
different work:

- **AVX2 is memory-bound.** 6.35 → 14.5 tok/s over 1.19–1.21 came from
  threading, adopting llama.cpp's kernel shape (Q8_K-style activation
  plane, integer-domain scaling, one float convert per superblock),
  prefetch distance tuning, and vectorising the activation quantiser
  and the DeltaNet recurrence. Further gains need fewer *bytes*.
- **MMX is instruction-bound.** `Q4_K` runs at **1.19 MMX instructions
  per weight**, and six of its nineteen per sixteen weights are
  `PUNPCK` widening that exists only because `PMADDWD` consumes words.
  MMX has no unsigned-byte multiply-accumulate — `PMADDUBSW` is SSSE3
  (2006) and Extended MMX on the Geode adds nothing that helps. Even
  driving `Q5_K` (1.88) and `Q6_K` (2.12) down to `Q4_K`'s 1.19 would
  be 1.37×. Further gains need less *work*, not faster instructions:
  lm-head requantisation or vocabulary pruning.

A caution that recurs throughout: **isolated kernel wins do not always
survive the whole model.** A Q5_K change that improved the kernel
0.103 → 0.086 ns/weight made the model *slower* (12.9 vs 13.3 tok/s),
and the Q6_K bias trick improved the kernel while costing 1.4% end to
end. Every change here was A/B'd against the full model.

---

## Two Geode facts that govern everything

Both measured by OLPC on real XO-1 silicon, not from the databook
(whose figures are throughputs and, per OLPC, "sometimes totally
wrong").

**1. The fast L1 window is 4 KB, not 64 KB.**

> "It adds two clocks of cache latency if the memory operand is not in
> last accessed way of data cache. Last way of L1 covers 4 KB."
> — 7-cpu.com

The L1 is 64 KB / 16-way, but only the last-accessed way is fast. Any
working set above ~4 KB pays +2 cycles per access.

**2. Every FPU load has a mandatory 2-cycle bubble.**

> "If the FPU loads from memory then the instruction which uses the
> target register must be scheduled 2 cycles after the FPU load."
> — OLPC

This is why MMX cannot approach its nominal 2-cycle throughput: every
`pmaddwd` with a memory operand is structurally delayed.

**3. IMUL costs ~16 cycles.** Not published anywhere; derived from the
measured i8 log (21.8 cycles/MAC measured, ~6 cycles of non-multiply
work per MAC at 1 IPC, leaving ~16 for the multiply).

---

## i8: the deep check

The actual inner loop, from `objdump` of the shipped `i486` build —
14 instructions producing 2 MACs:

```
.L134:
    movb   (%esi,%edx), %al          load weight byte
    movl   %eax, %ecx                copy
    andl   $15, %ecx                 low nibble
    movswl (%ebx,%edx,2), %ebp       load activation
    imull  %ebp, %ecx                MULTIPLY          ~16 cyc
    addl   %ecx, %edi                accumulate
    andl   $255, %eax                zero-extend
    sarl   $4, %eax                  high nibble
    movswl 64(%ebx,%edx,2), %ecx     load activation
    imull  %ecx, %eax                MULTIPLY          ~16 cyc
    addl   %eax, 12(%esp)            accumulate (SPILLED to memory)
    incl   %edx                      loop counter
    cmpl   $32, %edx                 compare
    jne    .L134                     branch
```

Cost model versus measurement:

| component | cycles per 2 MACs | share |
|---|---|---|
| 2x IMUL | 26 | **65%** |
| nibble extraction | 4 | 10% |
| 2 accumulates (one to memory) | 4 | 10% |
| loop overhead | 3 | 8% |
| 2 activation loads | 2 | 5% |
| weight load | 1 | 2% |
| **total** | **40 = 20.0 cyc/MAC** | |

Measured on hardware: **21.8 cycles/MAC**. The model is within 9%, so it
can be trusted to evaluate changes.

**IMUL is 65% of the i8 backend.** Everything else is noise by
comparison.

### Four attempts to remove it — all rejected

| approach | result |
|---|---|
| **Product lookup table** — tabulate all 16x256 products | Table is 8 KB, double the 4 KB fast-L1 window, so every access takes the +2 penalty. Measured **0.52x** on the reference host. |
| **Bucket accumulation** — `bk[nibble] += x`, then 15 multiplies to fold, replacing 32 multiplies with 32 adds | Modelled at Geode costs: **1.16x**. Measured on the reference host: **0.76x**. A 16% modelled gain, unverifiable without the hardware, against a large measured regression elsewhere — not worth shipping blind. |
| **Split loops** — one accumulator live at a time, to stop the spill | **0.81x**, and `objdump` shows it *still* spills. x86-32 simply does not have enough registers. |
| **CMOV** | See below — there is no conditional in the loop. |

### The floor

Per MAC the kernel must do, at minimum:

```
1 activation load      ~3
1 multiply (IMUL)     ~16
1 accumulate           ~1
share of weight work   ~2
----------------------------
                      ~22 cycles/MAC
```

Measured: **21.8**. There is no slack. The kernel is at its floor.

And even if IMUL were free, i8 would reach ~6 cycles/MAC ≈ 9 s/token —
still worse than MMX's current 15.8 s/token is... comparable at best.
`PMADDWD` does four MACs in one instruction with no multiply latency.
**A scalar backend cannot beat a SIMD one here.** i8 exists so the code
runs on a 486 without MMX, and that is the right reason for it to exist.

---

## Is CMOV worth using?

**No** — and the reason is structural, not marginal.

CMOV replaces a *branch* with a conditional *move*. It pays only where a
hot loop contains an unpredictable data-dependent branch.

The i8 inner loop above contains exactly one branch: `jne .L134`, the
loop back-edge. It is taken 31 times out of 32, perfectly predictable,
and CMOV cannot replace a back-edge anyway.

The MMX kernels contain no data-dependent branches either — the nibble
selection and hi-bit insertion were already made branch-free in 1.3.0
(`((h >> sh) & 1) << 4` rather than `(h & u) ? 16 : 0`).

Bounding it from the other side: the matvec loops execute roughly 56
million branches per forward pass. Even at an implausible 25%
misprediction rate and the measured 8-cycle penalty, that is **1.8% of
the cycle budget**. CMOV's entire addressable market here is under 2%,
and the actual addressable part is zero because there are no
unpredictable branches to remove.

(Related: the Geode *does* have a branch predictor — OLPC: "The IU has a
branch predictor of an unspecified size"; 7-cpu.com measured its
misprediction penalty. A penalty cannot exist without a predictor.)

---

## MMX: why it is also done

Four approaches were tried across four releases:

| release | change | Geode result |
|---|---|---|
| 1.3.0 | in-register unpacking, real Q6_K kernel (contributed) | **2.08x** |
| 1.4.0 | EMMS batched 16x, dependency-friendly scheduling | 1.04x |
| 1.5.0 | byte-domain masking, Q5_K 52→26 instructions | 1.05x (Q5_K alone 1.26x) |
| 1.6.0 | fused delta-net recurrence, 4 passes → 2 | 1.002x |

The pattern is unambiguous: the one big win came from removing
*redundant work*; everything since has been sub-5%.

Current state: **8.9 cycles/MAC**, ~3.2 cycles per MMX instruction
against a nominal ~2-cycle pipeline throughput. Given the mandatory
2-cycle FPU-load bubble, ~3.2 is close to the structural minimum.

### Why the 1.6.0 recurrence fusion did nothing (a miss worth recording)

It measured 1.52x on the reference host and **1.002x** on the Geode.

The state matrix `S` is 128x128 floats = **64 KB per head**. I reasoned
that four passes over a 64 KB structure through a 64 KB L1 would thrash,
and fusing to two passes would halve the traffic.

That was wrong because of Geode fact #1: the *fast* L1 window is 4 KB,
not 64 KB. Only 8 of S's 128 rows fit in it. 94% of S was already paying
the +2 cycle penalty regardless of how many passes were made, so halving
the passes halved a cost that was never the bottleneck.

The change is harmless (bit-identical output, fewer instructions) and
has been kept, but it should have been predicted to be neutral before
shipping.

### Multi-row: rejected

Reusing each activation load across 2 output rows was measured at
**0.90x**. MMX has 8 registers; `mm7` holds zero and 4 accumulators are
needed for 2 rows, leaving 3 scratch — not enough to keep both rows'
unpacked nibbles live, so the kernel re-loads each weight byte 4 times.
It trades 4 saved activation loads for 6 extra weight loads.

**Multi-row is an SSE2 optimisation (16 registers), not an MMX one.**

---

## i8 versus mmx: the whole history, same-version only

Comparing logs from different versions is meaningless, and doing so
produced a wrong claim in this project's own notes: `perf-i7-9th-i8.txt`
is **1.0.0** and `perf-i7-9th-mmx-1.6.0.txt` is **1.6.0**, six releases
apart. Read as a backend comparison it says i8 is 0.44x of mmx. It is
not; it is measuring six releases of MMX work.

Same version, same host, generation tok/s:

```
version / host    i8       mmx      i8/mmx
1.0.0 Geode       0.026    0.028    0.93x    <- neck and neck
1.0.0 i7          1.236    1.265    0.98x    <- neck and neck
1.5.0 Geode       0.030    0.063    0.48x
1.6.0 Geode       0.030    0.063    0.48x
1.16.0 Xeon       1.386    2.427    0.57x
```

**At 1.0.0 the two backends were within 2-7% of each other.** The gap
opened in 1.3.0-1.5.0, when the MMX kernels were rewritten (2.08x on the
Geode) while i8 gained comparatively little. It has since closed
slightly, 0.48x -> 0.57x, from the split-loop work in 1.15.3/1.16.0.

`ref` has always been far behind both. From `OPTIMIZATION_REPORT.md`
(32-bit build, `-n 60`): `ref` ~0.45 tok/s against i8 1.27 and mmx 1.92
— i8 is **2.8x** faster than ref end to end, and per-kernel:

```
Mflop/s   ref    i8     mmx      i8/ref
Q4_K      1449   2068   4272     1.43x
Q5_K      1308   1686   2853     1.29x
Q6_K       331   2108   3014     6.37x
Q8_0      2263   1718   2539     0.76x   <- ref wins here
```

Q6_K is where i8 earns its place: 6.4x over `ref`, because `ref`
re-sums 16 activations eight times per super-block while i8 uses the
precomputed `s16[]` sums. Q8_0 is the one format where `ref` is
faster, and it is 0.1% of runtime.

**The rule:** every backend comparison must come from one binary, one
run, one machine. Cross-version comparisons measure development, not
architecture.

## SPARC: cost per weight, not per call

Every SPARC log this project has collected, normalised by the number of
weight elements each format actually carries. Machine is an UltraSPARC
IIi at 440 MHz.

```
nanoseconds per weight        Q4_K      Q5_K      Q6_K     overall
i8   1.11.0  (32-bit)       618.05    975.41    508.35     646.01
i8   1.12.1  (64-bit)       590.05   1097.44    435.96     630.41
vis  1.14.2  (best of 3)    495.26   1151.72    426.90     599.86
vis  1.15.1  (see note)     614.17    704.56    525.18     596.75
```

Two things this makes visible that the raw profile hides.

**Q6_K is the cheapest format, and always has been.** Its calls carry
678,142 weights against Q4_K's 113,338 — six times as many — so
`us/call` makes it look six times more expensive than it is. It holds
`token_embd`, the LM head and the FFN down-projections. At 6.56 bits
per weight in a 210-byte super-block it also streams fewer bytes per
MAC than Q4_K's effective 4.5 bits plus scales, which matters more than
instruction count on a machine this memory-bound.

**Q5_K was the outlier.** 2.3x the per-weight cost of Q4_K for a format
only 25% wider. That gap was a redundant fold in the kernel, fixed in
1.15.1, and it closed to 1.15x.

The 1.15.1 row comes from a different session and is inflated across
the board — `rms norms`, untouched by any version, is 4.3x slower in
that log. Compare it only through within-run ratios:

```
cost per weight, relative to Q4_K in the same run
                      Q5_K     Q6_K
i8   1.11.0           1.58     0.82
i8   1.12.1           1.86     0.74
vis  1.14.2 (x3)      2.33     0.86-0.94
vis  1.15.1           1.15     0.86
```

Q5_K 2.33 -> 1.15 is the fold fix. Q6_K 0.86 -> 0.86 is the VIS kernel
buying nothing, which is why it is off by default.

## A methodological lesson

The reference host is out-of-order, has a 3-cycle IMUL, a 32 MB L3 and a
large fast L1. It mispredicts this machine *systematically*:

| change | reference host | Geode |
|---|---|---|
| 1.3.0 kernels | 1.72x | **2.08x** (under-reported) |
| i8 lookup tables | 2.46x isolated | **0.97x** (over-reported) |
| recurrence fusion | 1.52x | **1.002x** (over-reported) |

Microbenchmarks with everything in L1 are actively misleading here. The
only trustworthy measurements have come from the target hardware, and
every conclusion in this document is anchored to a measured log from it.

---

## What is actually left

Nothing in the kernels. What remains changes the workload:

1. **A smaller model or quantisation.** The only lever with a multiple
   in it. `Q4_K_S` cuts bytes ~10%; a 0.4B-class model roughly halves
   everything.
2. **Batched prompt ingestion.** Untouched. Affects time-to-first-token
   only (currently 3.1 min), but every prompt token presently re-unpacks
   the entire model. Plausibly several-fold on TTFT.
3. **Requantising `token_embd`** from Q6_K to Q4_K. The LM head is
   **18.1%** of runtime in only 11 calls, and that one tensor is 199 MB
   (40% of the file). Saves 63 MB of RAM as well.
