# Architecture

How `infer` is put together, and why. Written so that someone who has
never seen the code — human or machine — can find the right file, follow
a request end to end, and change something without breaking the rest.

---

## Contents

- [The shape of it](#the-shape-of-it)
- [Following one request all the way down](#following-one-request-all-the-way-down)
- [The layers](#the-layers)
- [The model: what qwen35 actually is](#the-model-what-qwen35-actually-is)
- [How a weight becomes a number](#how-a-weight-becomes-a-number)
- [The four compute backends](#the-four-compute-backends)
- [Byte order](#byte-order)
- [Ownership and lifetimes](#ownership-and-lifetimes)
- [Threading](#threading)
- [What to touch for a given change](#what-to-touch-for-a-given-change)

---

## The shape of it

```
                        opts.c
             one option table -> parsing,
             --help, applicability checks
                          |
                        main.c
                       dispatch
                          |
        +-----------------+-----------------+--------------+
        |                 |                 |              |
     serve             chat              run            info
   server.c          chat.c          (in main.c)    (in main.c)
        |                 |                 |
        +-----------------+-----------------+
                          |
                       agent.c
            messages -> prompt, tool-call parsing,
            tool invocation -- shared by all modes
                          |
                    +-----+-----+
                    |           |
                  mcp.c      jinja.c
                             jinja_eval.c
                          |
                     tokenizer.c
                          |
                    qwen35.c
                  the engine
                          |
            +-------------+-------------+
            |             |             |
       backend.c     sampler.c       gguf.c
            |                           |
      +-----+-----+                  sys.h
      |           |                (mmap/time)
   quant.c   backend_mmx.c
   (ref/i8)     (mmx)

    net.h  ----------------  used only by server.c and mcp.c
```

Two headers isolate every OS dependency:

* **`net.h`** — sockets. Implemented by `net_posix.c` or `net_win32.c`.
* **`sys.h`** — file mapping and a monotonic clock. `sys_posix.c` or
  `sys_win32.c`.

**Only those four files know an operating system exists.** Everything
else is portable C89. That is what makes the Windows XP and Solaris
ports small.

---

## Following one request all the way down

Take `POST /v1/chat/completions` with `{"messages":[...]}`.

**1. `net_posix.c` / `net_win32.c`** accepts the socket and hands back a
`net_conn *`. `server.c` never sees a file descriptor or a `SOCKET`.

**2. `server.c`** reads the request, parses headers, checks the API key,
routes on the path.

**3. `json.c`** parses the body into a `js_val` tree.

**4. `agent.c`** converts `messages` into `jj_val` form — the whole
message, not just role and content, so `tool_calls` and
`reasoning_content` survive — then applies the precedence rules
(`--system` fills in only if the request has none; request `raw` beats
`--raw`).

**5. `jinja_eval.c`** renders the model's chat template against that
context, producing the exact prompt string.

**6. `tokenizer.c`** encodes the prompt to token IDs with byte-level BPE.

**7. `qwen35.c`** runs the forward pass for each token. For every matvec
it calls into `backend.c`.

**8. `backend.c`** dispatches to the selected kernel — `mmx`, `i8` or
`ref` — which reads quantised weights straight out of the mmap'd blob.

**9. `sampler.c`** turns logits into a token (temperature, top-k, top-p,
repeat penalty).

**10.** Back up through `server.c`, which streams SSE chunks or builds
one JSON response, buffering partial UTF-8 so no chunk ever contains an
invalid sequence.

`infer chat` and `infer run` join at step 4 and take the same path from
there. That is deliberate: a regression test asserts that a conversation
built by `chat` and the same conversation arriving as JSON at `serve`
render to **byte-identical prompts**.

---

## The layers

### Container (`gguf.c`)

Reads the GGUF header, the key/value metadata block and the tensor
directory, then maps the tensor data.

Two details worth knowing:

**64-bit values without 64-bit integers.** The target is C89 on i486,
where `long long` is not guaranteed. File offsets are carried as
`double` (`gg_off`), exact for integers below 2^53 — far beyond any
model that fits in a 32-bit address space.

**The blob is mapped, not read.** `sys_map_file()` maps it read-only, so
weights are demand-paged: a machine with less RAM than the model still
runs, just slower, and nothing is written to swap. If mapping fails the
code falls back to `malloc` + `fread`.

Measured on a 508 MB Q4_K_M model during generation:

| | |
|---|---|
| peak RSS | ~561 MB |
| of which anonymous (irreducible) | ~62 MB |
| of which clean file-backed (reclaimable) | ~498 MB |

RSS is flat in context size, because 18 of 24 layers keep a fixed-size
recurrent state instead of a growing KV cache.

### Tokeniser (`tokenizer.c`)

Byte-level BPE. Independent of the engine — it needs only the GGUF
vocabulary.

Two behaviours that surprise people:

* A multi-byte UTF-8 character is often **split across tokens**. Any
  streaming path must buffer and emit only complete sequences, or
  clients (including the official OpenAI SDK) reject the output.
* The pre-tokeniser's `\p{N}` matches **one digit at a time**, so
  `1234567890` is ten tokens.

### Template engine (`jinja.c`, `jinja_eval.c`)

A Jinja2 subset, big enough for real chat templates: filters, tests,
`for`/`if`/`set`/`macro`, slices, `namespace()`, `tojson`.

Two rules are load-bearing, and both were bugs first:

* **`{{-` trims only literal template text** — never expression output,
  and never past the start of the current loop iteration. Getting this
  wrong deletes the newline after every message.
* **`|tojson` must sort object keys**, because Python's Jinja2 uses
  `json.dumps(sort_keys=True)`. Without it a tools prompt differs
  byte-for-byte from every other runtime.

The only reliable test for a template engine is byte-comparison against
Python. See [TEMPLATES.md](TEMPLATES.md).

### Prompt-prefix reuse

`qwen35_reuse_prefix()` lets `chat` and `serve` skip re-decoding a
prompt they have already seen the start of.

The hybrid architecture constrains this hard. The 6 attention layers
have a position-indexed KV cache that could be truncated anywhere; the
18 DeltaNet layers have a recurrent state that absorbed every token in
order and **cannot be rewound**, and snapshotting it costs ~18 MB per
token. So reuse is all-or-nothing: the whole history must be a prefix of
the new prompt, or the context resets.

Two things make it work in practice:

* the sampler is still fed every token, because repetition penalty is
  computed over the full sequence;
* `agent_tokenize_incremental()` tokenises the shared prefix separately,
  because byte-level BPE would otherwise assign different IDs to
  identical text (see finding 21).

`tests/t_cache.c` asserts that an incremental decode produces
bit-identical logits to a fresh one.

### Interfaces (`opts.c`, `agent.c`, `main.c`, `server.c`, `chat.c`)

The front ends are thin because two modules hold what they share:

* **`opts.c`** — the single option table. Each row names the modes it
  applies to, and that one declaration drives parsing, the generated
  `--help`, *and* the check that reports an option used where it has no
  meaning. Add a row, and the option is available, documented and
  validated everywhere at once.
* **`agent.c`** — messages to prompt, tool-call parsing, tool
  invocation. All three modes call it, so a fix to the tool-call parser
  fixes every mode simultaneously.

The rule these encode: **options are general unless the mode genuinely
cannot use them.** Only `--host`, `--port`, `--api-key`, `--alias`
(nothing else opens a socket) and `-p/--prompt` (`serve` takes its
prompt from the request body) are restricted.

---

## The model: what qwen35 actually is

This is the part a generic llama.cpp-style loop cannot run.

`general.architecture = "qwen35"` is a **hybrid**, not a plain
transformer. 24 blocks, of which:

* blocks **3, 7, 11, 15, 19, 23** — i.e. `(i+1) % 4 == 0` — are gated
  grouped-query **attention** layers, with a KV cache;
* the other **18** are **Gated DeltaNet** linear-attention layers, with
  a 128×128 recurrent state per head and **no KV cache at all**.

Key hyper-parameters for the 0.8B model:

| | |
|---|---|
| n_embd | 1024 |
| n_ff | 3584 |
| n_head / n_head_kv | 8 / 2 |
| head_dim | 256 |
| n_rot | 64, sections [11, 11, 10, 0] |
| rope base | 1e7 |
| rms_eps | 1e-6 |
| ssm_conv / ssm_state | 4 / 128 |
| ssm_group / ssm_dt_rank | 16 / 16 |
| ssm_inner | 2048 |
| full_attention_interval | 4 |
| n_vocab | 248320 |
| context_length | 262144 |

### The DeltaNet recurrence

With the state stored transposed (`S[j*hd+i] == state[i][j]`):

```
S *= exp(g)
S += k (x) (v - S^T k) * beta
out = S^T q / sqrt(head_dim)
```

`ssm_a` is already negative in the GGUF — it stores `-exp(A_log)`, so
the engine must not negate it again.

The practical consequence of the hybrid design: **memory does not grow
with context** the way it does for a pure transformer, but the recurrent
state must be carried between tokens, which is why `qwen35_reset()`
exists and why every mode resets before a new sequence.

---

## How a weight becomes a number

This is the hot path — 91% of runtime — so it is worth understanding
precisely.

### The file is not what its name says

`Qwen3.5-0.8B-Q4_K_M.gguf` is not a Q4_K file. By bytes:

| format | share |
|---|---|
| Q6_K | 46.9% |
| Q4_K | 32.7% |
| Q5_K | 19.9% |
| Q8_0 | 0.1% |

`token_embd` alone is Q6_K and 199 MB — 40% of the file — because the
embedding and the LM head are tied.

### The k-quant layout

A Q4_K block covers 256 weights in 144 bytes:

```
[ d: fp16 ][ dmin: fp16 ][ scales: 12 bytes ][ qs: 128 bytes ]
```

Twelve scale bytes encode eight 6-bit scale/min pairs. Each of the 128
`qs` bytes holds two 4-bit weights. Reconstructing weight `l` of
sub-block `j`:

```
w = d * scale[j] * nibble  -  dmin * min[j]
```

Q5_K adds a `qh` plane supplying a fifth bit per weight; Q6_K uses
`ql`/`qh` planes and a signed 8-bit scale with a −32 bias.

### Why the kernels are integer

The naive approach dequantises to float and multiplies. That is `ref`,
and it is slow: an fp16→fp32 conversion and a float multiply-add per
weight, on a machine whose FPU is asynchronous and needs synchronising.

`i8` instead quantises the **activations** to int8 once per matvec
(`bk_quantize_x`), then does the whole dot product in integers,
rescaling once per 32-weight block. The scale factors come out of the
sum, so the arithmetic is exact until the final float multiply.

`mmx` does the same thing four lanes at a time.

Measured on SPARC (per weight format, i8 vs ref): **6.5–8.8× faster**.
On the Geode with MMX: **2.08×** over the scalar path.

---

## The four compute backends

`backend.c` owns a table, ordered fastest-first, and picks the first one
the CPU supports:

```c
static const bk_backend backends[] = {
#ifdef INFER_HAVE_AVX2
    { "avx2", "...", bk_avx2_available, qmv_avx2 },
#endif
#ifdef INFER_HAVE_MMX
    { "mmx",  "...", mmx_ok,     qmv_mmx },
#endif
    { "i8",   "...", always_yes, qmv_i8  },
    { "ref",  "...", always_yes, qmv_ref }
};
```

| backend | where it runs | notes |
|---|---|---|
| `avx2` | x86 with AVX2 **and** OS `ymm` support | intrinsics, `backend_avx2.c`, the only object compiled `-mavx2`. Gated on CPUID *and* `XGETBV`. |
| `mmx` | x86 with MMX | GNU inline asm, `backend_mmx.c`, compiled only with `-DINFER_HAVE_MMX`. Runtime-gated on CPUID. |
| `i8` | **everywhere** | portable C89 integer kernels. Not x86-specific. |
| `ref` | everywhere | scalar float reference. Correctness baseline. |

Two things about this table are easy to get wrong:

**Registration keys on `INFER_HAVE_AVX2`, not `__AVX2__`.** `backend.c`
is compiled at the i486 baseline, so `__AVX2__` is false there even in
a build that links the AVX2 object. Testing the codegen macro instead
of the intent macro silently drops the kernel from every artifact —
which happened once, and is now asserted in CI. (Finding 50.)

**`auto` tries `avx2` before the `I8_IS_VECTORISED` preference.**
Finding 33 established "prefer the portable `i8` once the compiler can
vectorise", from a table whose only hand-written x86 entry was MMX at
4 MACs per instruction. `VPMADDUBSW` does 32, so that conclusion
inverts and `avx2` goes first; the finding-33 rule still applies
below it.

The only x86-specific code outside `backend_mmx.c` is the CPUID probe,
which is `#if defined(__i386__) || defined(__x86_64__)` guarded and
degrades to "no features detected".

### The 486-baseline trick

The MMX builds are compiled `-march=i486 -mtune=geode -mmmx`. The
baseline means no CMOV or other post-486 instruction is emitted for
ordinary code; `-mtune` only reorders. MMX appears solely in
`backend_mmx.c`, behind a runtime check.

**One binary therefore runs on a 486 and accelerates on a Geode.** With
`-march=geode` the compiler emits CMOV in `xmalloc`, `gguf_open` and
`main`, which faults on a 486.

### The alignment trap, and why there is a test for it

MMX constants are loaded with `movq`, which requires 8-byte alignment.
Declaring them as `short[4]` gives 4-byte alignment; x86 silently splits
the load, a Geode faults. A `union` with `long long` is **not**
sufficient — the i386 SysV ABI aligns `long long` to 4. The fix is an
explicit `__attribute__((aligned(8)))`, and `tests/t_align.c` asserts it
at run time so it cannot regress.

---

## Byte order

GGUF stores every multi-byte value little-endian. `infer` runs correctly
on big-endian hosts, verified on s390x and SPARC.

The reason it was nearly free: the i486 target had already forced
byte-wise access everywhere it matters.

* Header parsing (`rd_u16/u32/u64/i64`) assembles from bytes with shifts.
* `rd_f16p` — **the most-called wide read in the program** — is
  `p[0] | (p[1] << 8)`.
* Every k-quant kernel touches only `unsigned char`.

Only F32 tensors needed work, and F32 is 0.07% of the weights in a
k-quant model. `gguf_f32data()` returns the mapping directly on
little-endian; on big-endian it makes one swapped copy per tensor at
first use, because the blob is mapped read-only.

Measured cost of the whole thing: **0.35%**, i.e. noise.

### Detection refuses to guess

`INFER_BIG_ENDIAN` is derived from compiler predefines, preferring
`__BYTE_ORDER__` and falling back to a long list of vendor spellings
(`__sparc`, `__sparcv9`, `_BIG_ENDIAN`, `__hppa`, `mc68000`, ...).

If it cannot tell, the build **fails** with `#error` rather than
guessing. And `gguf_open()` calls `inf_check_byte_order()`, which
compares the macro against a runtime probe and decodes a known bit
pattern.

This exists because guessing wrong produced a program that loaded the
model, printed correct metadata, ran at full speed and emitted garbage —
see finding 20.

---

## Ownership and lifetimes

| object | owns | freed by |
|---|---|---|
| `gguf_file` | metadata, tensor directory, the mapping | `gguf_close` |
| `qwen35_model` | the `gguf_file`, layer table, tokeniser | `qwen35_free` |
| `qwen35_ctx` | activations, KV cache, recurrent state | `qwen35_ctx_free` |
| `jj_val` | children recursively | `jj_free` |
| `js_val` | children recursively | `js_free` |
| `strbuf` | its buffer | `sb_free` |
| `mcp_client` | tool list, connection state | `mcp_free` |

`jj_list_add` and `jj_dict_set` **take ownership** of the value passed.
`jj_dict_get` returns a **borrowed** pointer — clone it if you need to
keep it.

Tensor data is never copied. `gg_tensor.data` points into the mapping
and kernels read from there directly.

---

## Threading

**Opt-in, and off by default.** `--threads N` (`-T N`, `-T 0` for one
per core) splits work across a small persistent pool in `tpool.c`
(~230 lines over pthreads or Win32 events — no new library).

Two places are parallel, both because their units are independent:

- **matvec rows** (`q_matvec` in `backend.c`). Each output row is a
  separate dot product over shared read-only weights.
- **Gated DeltaNet heads** (`layer_deltanet` in `qwen35.c`). Each head
  owns its own 128×128 state and writes a disjoint slice of the output.

**Rows are split, never dot products**, so every value is summed by the
same code in the same order regardless of thread count — results are
bit-identical, asserted by `tests/t_thread.c` across backends and
thread counts.

Three constraints follow, and all three have been violated at least
once:

1. **No `static` scratch in any kernel.** It becomes shared mutable
   state the moment rows are split. The MMX kernels held their
   accumulator in a `static` and produced fluent gibberish under
   threading while every single-threaded test passed. (Finding 52.)
2. **Activation quantisation happens once, before the split.**
   `bk_quantize_x`/`bk_quantize_k` write one shared buffer, so
   `q_matvec` fills it and then latches it with `bk_quantize_hold()`
   for the duration of the parallel region.
3. **The default is 1.** The original default was one thread per core,
   which meant users who never asked for concurrency still got it —
   and in the MMX case, silent corruption. It also bought nothing on a
   2-core box (14.48 → 14.41 tok/s) because the memory bus was already
   saturated.

The server still handles one request at a time: the engine holds a
single recurrent state, so requests are serialised and each resets it.

---

## What to touch for a given change

| goal | files |
|---|---|
| add a command-line option | one row in `opts.c`'s table — parsing, help and validation follow |
| change prompt or tool behaviour | `agent.c`, once, for all three modes |
| add an API endpoint | `server.c` routing only |
| add a chat command | `chat.c` |
| extend the template engine | `jinja_eval.c`, then re-run the byte-comparison against Python |
| add a SIMD backend | new `backend_<isa>.c`, one row in `backend.c`'s table, one Makefile target |
| support another GGUF architecture | new `<arch>.c` beside `qwen35.c`, plus dispatch in `main.c` |
| port to a new OS | `net_<plat>.c`, `sys_<plat>.c`, a Makefile target — nothing else |
| add a quantisation format | `quant.c` (ref), then optionally `backend.c` (i8) and `backend_mmx.c` |

---

## See also

- [FINDINGS.md](FINDINGS.md) — 20 surprises, each with measurements
- [PERFORMANCE-ANALYSIS.md](PERFORMANCE-ANALYSIS.md) — why the kernels
  are at their hardware floor, including every rejected optimisation
- [TEMPLATES.md](TEMPLATES.md) — the Jinja subset and how to verify one
- [../COMPILE.md](../COMPILE.md) — every build target, step by step
- [files/](files/) — one document per source file
