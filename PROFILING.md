# Profiling infer

Two separate features. You can use either or both.

| | Enabled by | Cost | Tells you |
|---|---|---|---|
| **Throughput log** | `--log-perf` at run time | ~2 clock reads per request | TTFT, tokens/second and seconds/token, per request |
| **Stage profile** | `-DINFER_PROFILE` at build time **and** `--log-stages` at run time | ~2% (measured) | Where the time actually goes, per layer type and per weight format |

**Both are opt-in.** Building with `-DINFER_PROFILE` makes measurement
*possible*; `--log-stages` asks for it. Before 1.10.0 a profile build
printed a full report after every request whether or not you wanted one,
which made `infer-profile.exe` unpleasant for ordinary use — it is now
as quiet as any other build until you ask.

The stage profile is a **compile-time** option on purpose. Without
`-DINFER_PROFILE` every timer macro expands to nothing — verified with
`nm`, the profiling symbols are entirely absent from a normal build.
On a 500 MHz Geode a timer read inside the matvec loop would itself
distort what we are trying to measure, so the normal binaries carry no
instrumentation at all.

---

## Quick start (what I'd like you to run)

```sh
make linux PROFILE=1

./infer-<version>-linux run model.gguf \
    -p "What is the capital of France?" \
    -n 10 -t 0 -c 512 \
    --log-perf --log-stages --log-file perf-geode.txt
```

Then send me `perf-geode.txt`. That is the whole thing.

On Windows XP:

```
make windows PROFILE=1

infer-<version>-windows.exe run model.gguf -p "What is the capital of France?" -n 10 -t 0 -c 512 --log-perf --log-stages --log-file perf-xp.txt
```

On Solaris/SPARC:

```sh
gmake solaris PROFILE=1
./infer-<version>-solaris-profile run model.gguf -p "..." -n 10 -t 0 -c 512 \
    --log-perf --log-stages --log-file perf-sparc.txt
```

Use `-t 0` (greedy) so the run is deterministic and comparable. `-n 10`
is enough — at 30 s/token that is already a five-minute run, and the
per-stage percentages stabilise after two or three tokens.

**Run the versions you are comparing back to back, in one session.** A
log taken hours apart on a different machine state is not comparable:
one such pair showed `rms norms`, which no version had touched, running
4.3x slower, which invalidated every absolute number in it.

If you have the patience, a second run with `--backend i8` would let me
see exactly what the SIMD kernels are buying on real silicon:

```sh
./infer-<version>-linux run model.gguf -p "..." -n 10 -t 0 -c 512 \
    --backend i8 --log-perf --log-stages --log-file perf-geode-i8.txt
```

---

## Build targets

There are three targets and `PROFILE=1`:

```sh
make linux PROFILE=1      # -> infer-<version>-linux
make windows PROFILE=1    # -> infer-<version>-windows.exe
make solaris PROFILE=1    # -> infer-<version>-solaris-profile
```

`PROFILE=1` appends `-DINFER_PROFILE` and renames the binary, so a
profiling build can never overwrite a normal one. Without it the timers
are not compiled in at all — every `PROF_*` macro expands to nothing.

---

## Flags

```
--log-perf        log TTFT, tokens/second and seconds/token per request
--log-stages      print the per-stage profile at exit
                  (needs a -DINFER_PROFILE build; other builds say so)
--log-file <f>    append the log to <f> instead of stderr
```

Neither switch is implied by the other: `--log-perf` alone gives you
throughput without the stage table, and `--log-stages` turns on both,
since a stage profile without the corresponding rates is hard to read.

In `chat`, `--log-perf` prints a block after every turn and a **session
total** when you leave; `/perf` shows the running total at any point.

`--log-file` works with both features and in every mode. Under `serve`
you get a perf block per HTTP request, which is handy for watching how
TTFT scales with prompt length; under `chat`, one per turn plus the
session summary.

The log is flushed after every line, so if the machine is killed
mid-run you keep everything written so far.

---

## Reading the output

### Header

```
infer 1.1.0 -- performance log
started      : Sat Aug  1 23:02:27 2026
model        : Qwen3.5-0.8B-Q4_K_M.gguf
backend      : mmx
cpu          : AuthenticAMD  features: mmx 3dnow cmov
context      : 512 tokens
stage profile: compiled in (-DINFER_PROFILE)
pointer size : 32 bits
```

Confirms which backend was actually selected and what the CPU reported —
useful when a result is surprising.

### Throughput block (one per request)

```
--- perf: run ---
  prompt      :    19 tok in 3.80 s    (4.995 tok/s, 0.20 s/tok)
  generation  :     8 tok in 3.01 s    (2.660 tok/s, 0.38 s/tok)
  TTFT        : 3.80 s
  total       : 6.81 s
```

`TTFT` is time from request start to the first output token, which on
this architecture is essentially prompt-ingestion time. Prompt and
generation rates are reported separately because they differ: prompt
tokens skip the LM head except on the last one.

### Stage profile (once, at exit)

```
STAGE PROFILE  (21 forward passes)
stage                       seconds       %     calls  us/call
TOTAL forward pass           12.616  100.0%        21 600755.1
  embedding lookup            0.000    0.0%        21     10.8
  attention layers (6)        0.715    5.7%       126   5672.5
    qkv projections           0.503    4.0%       126   3993.5
    scores+softmax+AV         0.015    0.1%       126    120.5
    output projection         0.190    1.5%       126   1509.7
  delta-net layers (18)       5.139   40.7%       378  13596.4
    input projections         3.626   28.7%       378   9592.5
    causal conv + SiLU        0.162    1.3%       378    427.3
    state recurrence          0.292    2.3%       378    772.4
    output projection         1.057    8.4%       378   2795.5
  feed-forward (24)           4.674   37.1%       504   9274.3
  rms norms                   0.003    0.0%      1008      2.6
  lm head                     2.084   16.5%         6 347253.5
--------------------------------------------------------
by weight format (overlaps the stages above)
matvec Q4_K                   4.518   35.8%      2058   2195.2
matvec Q5_K                   4.056   32.1%       756   5364.7
matvec Q6_K                   3.420   27.1%       342  10000.1
--------------------------------------------------------
matvec throughput      : 4971.4 Mflop/s
weights streamed       : ~333 MB per forward pass
time per forward pass  : 0.252 s
```

Indentation shows nesting: the sub-stages under `attention layers` are
included in its total, not additional to it. The `by weight format`
section is a second, orthogonal view of the same time — it slices the
matvec cost by quantisation format instead of by network stage, which
is how you tell whether a particular quant is the problem.

`calls` matters: 6 attention layers x N forward passes, 18 delta-net
layers x N, and so on. `us/call` is the per-layer cost, which is the
number to compare across machines.

---

## What a Geode-class run should show

Predictions, recorded before the fact so they can be checked against
reality:

1. **FFN should dominate**, ~40%. It is 3 matvecs of 1024x3584 per
   layer across all 24 layers — by far the most weight bytes touched.

2. **Delta-net input projections should be second.** That is
   `attn_qkv` (1024x6144, Q5_K) in 18 layers.

3. **The LM head should be large but rare** — one 1024x248320 Q6_K
   matvec, only on tokens where logits are needed. On the 64-bit host it
   is ~168 ms per call. On the Geode it may well be several seconds,
   which would make it the single most expensive *operation* even though
   it is a small share of the total.

4. **`state recurrence` should be small** (~2-6%). If it is much larger
   on the Geode, the 128x128 state matrices (64 KB per layer) are
   thrashing the 64 KB L1, and blocking that loop would be worth doing.

5. **Q6_K should look bad per call.** It is the format the MMX backend
   delegates to scalar code, and `ffn_down` in half the layers uses it.
   If Q6_K is disproportionate on real hardware, converting the model to
   a quant that avoids Q6_K, or writing a proper MMX Q6_K kernel, moves
   the needle.

If the measured split differs from this, the profile is indicating
something I got wrong — which is exactly why it is worth measuring
rather than guessing.

---

## Caveats

* `weights streamed` is an estimate derived from MAC counts and an
  average bits-per-weight; treat it as an order of magnitude, not a
  measurement.
* `Mflop/s` counts multiply-accumulates as 2 flops, matching the
  convention in `t_backend`.
* Timings are wall clock (`gettimeofday` on POSIX,
  `QueryPerformanceCounter` on Win32), so anything else running on the
  machine shows up in the numbers. Close other programs first.
* Sub-stage timers add a clock read per layer per token — 24 layers x
  ~6 timers. At 30 s/token that is utterly negligible; on the fast host
  it measured as ~2%, within run-to-run noise.
