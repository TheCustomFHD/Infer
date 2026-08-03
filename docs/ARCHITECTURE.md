# Architecture — how the pieces fit

How data flows through `infer`, why the boundaries are where they are,
and what depends on what.

Per-file detail lives in [files/](files/). Start here.

---

## The shape of it

```
                        opts.c
             one option table -> parsing,
             --help, applicability checks
                          │
                        main.c
                       dispatch
                          │
        ┌─────────────────┼─────────────────┬──────────────┐
        │                 │                 │              │
     serve             chat              run            info
   server.c          chat.c          (in main.c)    (in main.c)
        │                 │                 │
        └─────────────────┼─────────────────┘
                          │
                       agent.c
            messages -> prompt, tool-call parsing,
            tool invocation -- shared by all modes
                          │
                    ┌─────┴─────┐
                    │           │
                  mcp.c      jinja.c
                             jinja_eval.c
                          │
                     tokenizer.c
                          │
                    qwen35.c
                  the engine
                        │
            ┌───────────┼───────────┐
            │           │           │
       backend.c    sampler.c    gguf.c
            │                       │
      ┌─────┴─────┐              sys.h
      │           │            (mmap/time)
   quant.c   backend_mmx.c
   (ref)        (mmx)

    net.h  ──────────────────────  used only by server.c and mcp.c
```

Two abstraction headers isolate every OS dependency:

* **`net.h`** — sockets. Implemented by `net_posix.c` or `net_win32.c`.
* **`sys.h`** — file mapping and the monotonic clock. `sys_posix.c` or
  `sys_win32.c`.

Nothing else in the project includes an OS header. That is why Windows
XP support cost two files rather than a fork.

---

## A request, end to end

Take `POST /v1/chat/completions`:

**1. `net_posix.c` / `net_win32.c`** accept the connection and hand back
an opaque `net_conn`.

**2. `server.c`** reads the HTTP request, parses headers, checks the API
key.

**3. `json.c`** parses the body into a `js_val` tree.

**4. `server.c`** converts `messages` into `jj_val` form and calls the
template engine.

**5. `jinja_eval.c`** renders the model's chat template — the one from
the GGUF, or a `--template` override — producing the exact prompt text.

**6. `tokenizer.c`** turns that text into token IDs: special-token scan,
regex pre-split, byte-level BPE.

**7. `qwen35.c`** runs each token through the network. Per block it calls
either the attention path or the delta-net path, then the FFN.

**8. `backend.c`** dispatches every matrix-vector product to the selected
kernel — `ref` (`quant.c`), `i8` (`backend.c`), or `mmx`
(`backend_mmx.c`). **This is ~95% of the time spent.**

**9. `sampler.c`** picks the next token from the logits.

**10.** Steps 7–9 repeat. Each token is detokenised and streamed back
through `server.c` → `net.h`.

`infer chat` follows the same path from step 4, with `mcp.c` inserted
after step 10 when the reply contains a tool call.

---

## Layers, bottom up

### Platform (`net.h`, `sys.h`)

Twelve networking functions, four mapping/time functions. Opaque
handles; no `sockaddr`, no `errno`, no `HANDLE` crosses the boundary.

To port: implement these two files. Nothing above changes.

### Container (`gguf.c`)

Parses the GGUF header, metadata and tensor directory, then maps the
tensor blob read-only through `sys.h`. Falls back to `malloc`+`fread` if
mapping is unavailable.

Uses `double` for all file offsets — C89 has no 64-bit integer and
`long` is 32 bits on i486.

### Compute (`quant.c`, `backend.c`, `backend_mmx.c`)

`backend.c` owns the public `q_matvec()` and picks a kernel by CPUID at
startup. `quant.c` holds the float reference and the block decoders;
`backend_mmx.c` holds the SIMD versions.

The split matters: `--backend ref` is always available as ground truth
when a fast kernel is suspected.

### Engine (`qwen35.c`)

The `qwen35` hybrid architecture — 6 gated-attention layers, 18 Gated
DeltaNet layers. Opaque `qwen35_model` and `qwen35_ctx`; a context owns
all activation scratch, the KV cache and the recurrent state, allocated
once.

### Text (`tokenizer.c`, `jinja*.c`, `json.c`)

Independent of the engine. The tokeniser needs only the GGUF vocab; the
template engine needs only its own value type.

### Interfaces (`server.c`, `chat.c`, `main.c`)

Three front ends over the same engine. `main.c` contains no inference and
no networking.

They are deliberately thin, because two modules underneath them hold
everything the modes have in common:

* **`opts.c`** — the single option table. Each entry names the modes it
  applies to, and that one declaration drives parsing, the generated
  `--help`, and the check that reports an option used where it has no
  meaning. Adding a toggle in one place makes it available, documented
  and validated everywhere at once.
* **`agent.c`** — messages to prompt, tool-call parsing, tool
  invocation. Because all three modes call it, `--system`, `--think`,
  `--raw`, `--template` and `--mcp` behave identically in each, and a
  fix to the tool-call parser fixes every mode simultaneously.

The rule these two encode: **options are general unless the mode
genuinely cannot use them.** Only `--host`, `--port`, `--api-key` and
`--alias` (no socket outside `serve`) and `-p/--prompt` (`serve` gets its
prompt from the request) are restricted.

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

`jj_list_add` and `jj_dict_set` **take ownership** of the value passed.
`jj_dict_get` returns a borrowed pointer — clone it if you need to keep
it.

Tensor data is never copied. `gg_tensor.data` points into the mapping,
and kernels read from there directly.

---

## Threading

None. Single-threaded throughout, deliberately: the target is a
single-core in-order 500 MHz CPU where threads would add complexity and
contention for no gain.

The server handles one request at a time. The engine holds a single
recurrent state, so requests are serialised and each resets that state.

---

## What to touch for a given change

| goal | files |
|---|---|
| support another GGUF architecture | new `<arch>.c` beside `qwen35.c`, plus dispatch in `main.c` |
| add an SSE2 backend | new `backend_sse2.c`, one entry in `backend.c`'s table |
| port to a new OS | `net_<plat>.c`, `sys_<plat>.c`, a Makefile target |
| add an API endpoint | `server.c` routing only |
| extend the template engine | `jinja_eval.c`, then re-run the byte-comparison |
| add a chat command | `chat.c` |
| add a command-line option | one row in `opts.c`'s table — parsing, help and validation follow |
| change prompt or tool behaviour | `agent.c`, once, for all three modes |

---

## See also

- [files/](files/) — one document per source file
- [FINDINGS.md](FINDINGS.md) — the surprises, with measurements
- [../COMPILE.md](../COMPILE.md) — every build target, step by step
