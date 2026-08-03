# `src/prof.c`, `src/prof.h` — profiling and throughput logging

**~410 lines.** Two independent features.

## 1. Throughput logging — run time, always compiled

`--log-perf` reports time-to-first-token and tokens/second per request:

```
--- perf: run ---
  prompt      :    19 tok in 3.80 s    (4.995 tok/s, 0.20 s/tok)
  generation  :     8 tok in 3.01 s    (2.660 tok/s, 0.38 s/tok)
  TTFT        : 3.80 s
  total       : 6.81 s
```

Costs two clock reads per request, so it is always available. Works with
`serve` too — one block per HTTP request.

## 2. Stage profiling — compile time only

`-DINFER_PROFILE` (via `make profile*`) adds per-stage timers:
embedding, attention and its sub-stages, delta-net and its sub-stages,
FFN, norms, LM head, plus a second view sliced by weight format.

**Without the define every macro expands to nothing.** Verified with
`nm`: `prof_add` and `prof_now` are entirely absent from a normal build.
That matters — on a 500 MHz Geode a `gettimeofday()` inside the matvec
loop would distort exactly what is being measured.

```c
#define PROF_DECL(v)            /* nothing */
#define PROF_START(v)           ((void)0)
#define PROF_STOP(v, stage, w)  ((void)0)
```

Measured overhead when enabled: ~2%, within run-to-run noise.

## Output

`--log-file <f>` writes to a file instead of stderr, flushed after every
line so a killed run still leaves usable data. The banner records model,
backend, CPU vendor and features, context size and pointer width — all
the things needed to interpret a log someone emails you.

## Both features are opt-in

`prof_perf_enabled` (from `--log-perf`) and `prof_stages_enabled` (from
`--log-stages`) are separate run-time switches. A `-DINFER_PROFILE`
build compiles the stage timers in but prints nothing until asked.

Until 1.10.0 `main.c` set `prof_perf_enabled = log_perf || PROF_ENABLED`,
so the profile binaries reported after every request unconditionally.
Building for measurement and requesting measurements are different
decisions, and conflating them made `infer-profile.exe` unpleasant to
use as an ordinary binary.

`--log-stages` on a build without `-DINFER_PROFILE` is reported rather
than ignored — there is genuinely no data to print, and saying so beats
printing nothing.

## Session totals

`perf_total` accumulates several `perf_run`s so `chat` can report
tokens/second and seconds/token for a whole conversation, not only the
last turn. `/perf` prints the running total mid-session.

## See also

- [../PROFILING.md](../../PROFILING.md) ·
  [../PERFORMANCE-ANALYSIS.md](../PERFORMANCE-ANALYSIS.md)
