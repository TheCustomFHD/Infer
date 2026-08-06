# `src/tpool.c` — the thread pool

**~230 lines.** A minimal persistent pool over the platform's own
primitives: pthreads on POSIX, `CreateThread` + events on Win32. No
OpenMP, no new library — the project ships to Sun Studio on Solaris,
mingw on Windows XP, and a 486 with no threads at all.

Without `INFER_HAVE_THREADS` every entry point is a stub and the whole
facility compiles away (`make linux NO_THREADS=1`). That is the 486
build.

## The contract

```c
void tp_parallel_for(long n, tp_fn fn, void *arg);
```

Calls `fn(arg, lo, hi)` on disjoint sub-ranges covering `[0, n)` and
returns when all are done. **The calling thread runs one of the slices
itself**, so with one thread this is exactly `fn(arg, 0, n)` with no
synchronisation at all.

**Results are bit-identical to the serial path.** Index `i` is computed
by the same code summing the same terms in the same order, just
possibly on another core. This is not a reassociation — no partial sum
crosses a thread. `tests/t_thread.c` asserts it across every backend
and several thread counts, including row counts that do not divide
evenly.

## Where it is used

Exactly two call sites, both because their units are independent:

- **`q_matvec`** (`backend.c`) — matvec output rows. Shared weights and
  activations are read-only.
- **`layer_deltanet`** (`qwen35.c`) — Gated DeltaNet heads. Each owns
  its own 128×128 state and writes a disjoint output slice.

Only two worker bodies exist: `mv_slice` and `dn_recur_slice`. Anything
those can reach must be thread-safe; anything they cannot is not a
concern. (Checked: the remaining `static` scratch in `bpe_word` is
unreachable — tokenisation completes before the first forward pass.)

## Three hazards, all of which have bitten

**1. `static` scratch in a kernel.** The MMX kernels held their
per-row accumulator in a `static int acc[]`, chosen originally to keep
1 KB off a 486's stack. Once rows were split, every worker wrote the
same array. Each dot product was still individually correct, so
`t_ident` and a full synthetic shape sweep both passed — and the model
emitted fluent gibberish. Accumulators are now automatic, and CI greps
the backend sources for `static` arrays. (Finding 52.)

**2. Shared activation quantisation.** `bk_quantize_x` and
`bk_quantize_k` write one shared scratch buffer. `q_matvec` fills it
*before* the split and then latches it with `bk_quantize_hold()`, so
the workers' own calls become no-ops instead of racing. An earlier
attempt guessed whether `x` had changed from a pointer and two sampled
values — unsafe, because `x` lives in a buffer rewritten in place, so a
false hit computes with stale activations.

**3. Per-head scratch sized for one head.** The DeltaNet recurrence
wrote `kv_delta[hd]`, one shared row. Threading the head loop needs
`[n_heads][hd]`.

## Why the default is one thread

`--threads` defaults to **1**; `-T 0` means one per logical CPU.

It is a throughput trade, not a free win: this workload streams the
whole model once per token, and on a 2-core machine the memory bus was
already saturated at one thread (14.48 → 14.41 tok/s, i.e. nothing).
Defaulting to every core also meant users who never asked for
concurrency still got it — and in the MMX case, silent corruption.

## See also

- [`backend-c.md`](backend-c.md) — the matvec split
- [`qwen35-c.md`](qwen35-c.md) — the DeltaNet head split
- [`../FINDINGS.md`](../FINDINGS.md) — finding 52
