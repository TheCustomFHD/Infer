# Architecture

How a token is produced, and why the code looks the way it does.

- [The shape of it](#the-shape-of-it)
- [One request, all the way down](#one-request-all-the-way-down)
- [The model: what qwen35 is](#the-model-what-qwen35-is)
- [How a weight becomes a number](#how-a-weight-becomes-a-number)
- [The compute backends](#the-compute-backends)
- [The 486-baseline trick](#the-486-baseline-trick)
- [Threading](#threading)
- [Byte order](#byte-order)
- [Ownership and lifetimes](#ownership-and-lifetimes)
- [What to touch for a given change](#what-to-touch-for-a-given-change)

---

## The shape of it

```
                  main.c  — parse argv, dispatch on the verb
                    |
      +-------------+-------------+-------------+
      |             |             |             |
   server.c      chat.c         run           info
   (HTTP)        (REPL)      (one-shot)    (metadata)
      |             |             |
      +------ agent.c -----------+     shared prompt building,
      |       (one place)              tool calling, sampling setup
      |
   jinja.c / jinja_eval.c            render the GGUF chat template
      |
   tokenizer.c                       text  <-> token ids
      |
   qwen35.c                          the engine: layers, KV cache,
      |                              recurrent state
      |
   backend.c                         pick a matvec kernel, quantise
      |                              activations, split rows
      |
   backend_avx2.c / backend_mmx.c / backend_vis.c / qmv_i8 / qmv_ref
      |
   quant.c + gguf.c                  dequantise, mmap the file
```

Two rules hold this together:

- **One option table.** `opts.c` records every flag, which modes
  accept it, and its help text. Parsing, `--help` and validation all
  derive from it, so a flag cannot exist in one and not the others.
- **One prompt builder.** `agent.c` renders messages and parses tool
  calls for `serve`, `chat` and `run` alike. `tests/t_agent.c` asserts
  the three modes produce **byte-identical** prompts.

## One request, all the way down

`POST /v1/chat/completions` with `{"messages":[…]}`:

1. **`server.c`** reads the request, checks the bearer token if
   `--api-key` was set, parses JSON (`json.c`).
2. **`agent.c`** converts the JSON messages to Jinja values and merges
   CLI defaults (`--system`) with per-request overrides.
3. **`jinja.c`/`jinja_eval.c`** render the model's own chat template
   into one prompt string.
4. **`tokenizer.c`** turns that into token ids (byte-level BPE).
5. **Prompt-prefix reuse**: if this prompt shares a prefix with the
   last one, only the new tail is ingested. `tests/t_cache.c` asserts
   the resulting logits are bit-identical to a fresh decode.
6. **`qwen35.c`** runs the forward pass per token: 24 blocks, then the
   LM head.
7. **`sampler.c`** applies repetition penalty, top-k, top-p and
   temperature; `-t 0` short-circuits to argmax.
8. **`server.c`** streams the piece back as SSE, or accumulates and
   returns one JSON body.

Steps 6–7 repeat until an EOS token, a stop string, or `-n`.

## The model: what qwen35 is

**Not a plain transformer.** It is a hybrid, and that is the single
most important fact about this codebase.

24 blocks. Blocks where `(index + 1) % 4 == 0` — that is 3, 7, 11, 15,
19, 23 — are **gated GQA attention** with a KV cache. The other
**18 are Gated DeltaNet**, a linear-attention layer with a recurrent
state and *no* KV cache.

Hyper-parameters for the 0.8B model, read from the GGUF (defaults in
`qwen35.c` shown):

| key | value |
|---|---|
| `embedding_length` | 1024 |
| `feed_forward_length` | 3584 |
| `attention.head_count` | 8 |
| `attention.head_count_kv` | 2 |
| `attention.key_length` (head dim) | 256 |
| `rope.dimension_count` | 64 |
| `ssm.conv_kernel` | 4 |
| `ssm.state_size` (head dim) | 128 |
| `ssm.group_count` (k/q heads) | 16 |
| `ssm.time_step_rank` (v heads) | 16 |
| `ssm.inner_size` | 2048 |
| `full_attention_interval` | 4 |
| vocabulary | 248,320 |

Consequences that shape everything else:

- **Memory does not grow with context in 18 of 24 layers.** Those hold
  a fixed 128x128 state per head — 18.9 MB total — regardless of how
  long the conversation is. Only the 6 attention layers have a KV
  cache.
- **The vocabulary is enormous.** At 248,320 x 1024 in Q6_K the LM head
  is **208.6 MB**, which is 39% of the file and 27% of the bytes moved
  per generated token. It is the single biggest tensor by far.

### The DeltaNet recurrence

Per value head, per token, over a `hd x hd` state matrix `S`:

```
S = S * decay                    (decay from the gate)
kv = (v - S·k) * beta            (the "delta" correction)
S = S + kv (x) k                 (outer product update)
o = (S·q) * scale
```

Written as two fused "update the row, then dot it" passes. GCC will
not vectorise a read-modify-write feeding a reduction — verified, zero
ymm emitted — so the AVX2 form is written out by hand in
`backend_avx2.c`. It was 10.9% of the forward pass before that.

## How a weight becomes a number

### The file is not what its name says

`Q4_K_M` is a *mix*. In this model the tensors are Q4_K, Q5_K, Q6_K
and Q8_0, chosen per tensor by the quantiser. Any kernel work must
cover all four, and the profile is dominated by whichever the big
tensors happen to use — here Q6_K, because that is what the LM head
is.

### The k-quant layout

A k-quant super-block is 256 weights. For Q4_K that is 144 bytes:

```
[ d: fp16 ][ dmin: fp16 ][ scales: 12 bytes ][ qs: 128 bytes ]
```

The 12 scale bytes pack **six 6-bit scales and six 6-bit mins**. The
weight is `d * scale * q - dmin * min`. Decoding those packed 6-bit
fields turned out to cost more than the arithmetic they feed — see
[04-PERFORMANCE.md](04-PERFORMANCE.md).

### Why the kernels are integer

The activations are quantised to int8 once per matvec, then the whole
dot product runs in integer arithmetic and converts to float once per
super-block. This is what makes a 486 or a Geode viable at all: no
per-weight float multiply, and MMX/VIS/AVX2 all have wide integer
multiply-accumulate.

Two activation planes exist because the kernels want different things:

- **per-32 plane** (`bk_quantize_x`) — one scale per 32 values, used
  by `Q8_0` and the portable paths.
- **Q8_K-style per-256 plane** (`bk_quantize_k`) — one scale per 256
  values plus per-16 sums, used by the AVX2 k-quant kernels so the
  weight scale can be folded into an int32 accumulator spanning a
  whole super-block.

Only the plane a format needs is built, lazily. Building both on every
call meant two full passes over the activations for ~187 matvecs per
forward pass.

## The compute backends

`backend.c` owns a table, ordered fastest-first, and picks the first
one the CPU supports:

```c
static const bk_backend backends[] = {
#ifdef INFER_HAVE_AVX2
    { "avx2", "...", bk_avx2_available, qmv_avx2 },
#endif
#ifdef INFER_HAVE_VIS
    { "vis",  "...", bk_vis_available,  bk_qmv_vis },
#endif
#ifdef INFER_HAVE_MMX
    { "mmx",  "...", mmx_ok,     qmv_mmx },
#endif
    { "i8",   "...", always_yes, qmv_i8  },
    { "ref",  "...", always_yes, qmv_ref }
};
```

`vis` and `mmx` are never both present — one is SPARC, the other x86.
An x86 build offers four backends, a Solaris build three.

| backend | where it runs | notes |
|---|---|---|
| `avx2` | x86 with AVX2 **and** OS `ymm` support | intrinsics; the only object compiled `-mavx2`. **Not bit-identical** to the others on k-quants — bounded by `t_avx2acc` |
| `vis` | UltraSPARC with VIS 1 | `backend_vis.c`. Bit-identical to `i8` |
| `mmx` | x86 with MMX | GNU inline asm. Bit-identical to `i8` |
| `i8` | everywhere | portable C89 integer kernels |
| `ref` | everywhere | scalar float reference; correctness baseline |

The AVX2 gate checks CPUID **and** `XGETBV`. Windows XP never saved
the `ymm` registers across a context switch, so an XP machine on
Haswell silicon reports AVX2 in CPUID and cannot execute it; checking
only CPUID would crash instead of falling back to `mmx`.

Runtime selection costs one indirect call per matrix *row* — measured
at the noise floor.

## The 486-baseline trick

The 32-bit builds are compiled `-march=i486 -mtune=generic -mno-sse
-mno-sse2 -mfpmath=387 -mmmx`; the 64-bit ones just `-mtune=generic`.
The baseline means no CMOV or other post-486 instruction is emitted
for ordinary code; `-mtune` only reorders.

**One binary therefore runs on a 486 and accelerates on a Geode or a
Haswell.** With `-march=geode` the compiler emits CMOV in `xmalloc`,
`gguf_open` and `main`, which faults on a 486. Measured cost of the
i486 baseline versus letting the compiler use CMOV: **0.6%**, in
favour of the baseline.

### The alignment trap

MMX constants are loaded with `movq`, which requires 8-byte alignment.
Declaring them `short[4]` gives 4-byte alignment; x86 silently splits
the load and a Geode faults. A `union` with `long long` is **not**
sufficient — the i386 SysV ABI aligns `long long` to 4. The fix is an
explicit `__attribute__((aligned(8)))`, and `tests/t_align.c` asserts
it at run time so a compiler that ignores the attribute is caught by
`make test` rather than by a user's crash.

## Threading

**Opt-in, off by default.** `--threads N` splits work across a small
persistent pool in `tpool.c` (~450 lines over pthreads or Win32
events — no new library).

Two places are parallel, both because their units are independent:

- **matvec rows** (`q_matvec` in `backend.c`) — each output row is a
  separate dot product over shared read-only weights.
- **Gated DeltaNet heads** (`layer_deltanet` in `qwen35.c`) — each head
  owns its own 128x128 state and writes a disjoint output slice.

**Rows are split, never dot products**, so every value is summed by
the same code in the same order regardless of thread count. Results
are bit-identical; `tests/t_thread.c` asserts it across every backend
by name.

Three constraints, all of which have been violated at least once:

1. **No `static` scratch in any kernel.** It becomes shared mutable
   state the moment rows are split. The MMX kernels held their
   accumulator in a `static` and produced fluent gibberish under
   threading while every single-threaded test passed.
2. **Activation quantisation happens once, before the split.**
   `q_matvec` fills the shared buffer then latches it with
   `bk_quantize_hold()` for the parallel region.
3. **The default is 1.** A default that quietly uses every core is
   wrong here — and in the MMX case it meant silent corruption.

### The barrier must be cheaper than the work it guards

A forward pass issues about **95** parallel regions (77 matvecs with
enough rows, plus 18 DeltaNet head loops). Until 1.23.0 each was a
condition-variable round trip costing **44–48 us**, leaving two cores
at 159% CPU with 17,402 voluntary context switches per 58 passes.
Shapes below ~1024 rows were *slower* threaded than serial.

The pool now polls a generation counter before blocking. The barrier
costs **0.25 us**, and `-T 2` went 17.25 → 22.16 tok/s.

**The spin is conditional and that is not optional.** With more
threads than cores, spinners starve the workers they wait on (`-T 4`
on 2 cores measured 17.2 → 10.4 with an unconditional spin).
`tp_start` sets the budget from `nthreads <= tp_cpu_count()`, and at
zero **both** sides take the original blocking path — the old code,
not a degraded imitation.

**Both platform paths must re-check in a loop.** The Win32 worker
used a bare `if` around `WaitForSingleObject` while pthread used
`while`. Because `tp_go` is an auto-reset event signalled every batch,
a signal latched during spinning made the next wait return with no new
work; the worker re-ran the previous slice and Windows crashed at
`-T 2`. Guarded in CI now.

The server still handles one request at a time: the engine holds a
single recurrent state.

## Byte order

GGUF stores every multi-byte value little-endian. `infer` runs
correctly on big-endian hosts, verified on s390x and SPARC.

Detection **refuses to guess**: the `#if` ladder tests every vendor
spelling it knows, and the `#else` is an `#error`. A portability macro
that guesses converts a build error into eight minutes of garbage
output on an UltraSPARC — which is exactly what happened.

The one endianness bug that shipped was really a **type-width** bug:
`q_fp16_to_fp32()` was cleared as "endianness-neutral" by a source
audit and was the actual defect.

## Ownership and lifetimes

- The model file is **`mmap`ed read-only** and never copied. Tensor
  data are pointers into that mapping.
- `MADV_SEQUENTIAL` + `MADV_WILLNEED` are advised once at load.
  `MADV_RANDOM` was there originally and cost real throughput — this
  workload walks every weight in address order once per token, the
  most sequential pattern there is.
- The engine context owns all scratch: activations, KV cache,
  recurrent state, quantisation planes. Allocated once at load, sized
  from the hyper-parameters, freed at exit.
- Nothing in the hot path allocates.

## What to touch for a given change

| goal | files |
|---|---|
| add a command-line option | one row in `opts.c`'s table — parsing, help and validation follow |
| change prompt or tool behaviour | `agent.c`, once, for all three modes |
| add a weight format | `quant.c` (dequantise) + each backend's kernel + `t_ident` |
| add a compute backend | new `backend_<isa>.c`, one row in the table in `backend.c`, a gated object in the Makefile |
| change the HTTP surface | `server.c` |
| change sampling | `sampler.c` |
| change the model architecture | `qwen35.c` |
| change threading | `tpool.c` — and read [06-PITFALLS.md](06-PITFALLS.md) first |
