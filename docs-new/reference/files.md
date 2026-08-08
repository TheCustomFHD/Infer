# Source file reference

One line per file, plus where to go for a given change. Line counts
are approximate and will drift; the descriptions should not.

## Entry and interfaces

| file | role |
|---|---|
| `main.c` | parse argv, dispatch on the verb, wire up logging |
| `opts.c` / `opts.h` | **the** option table: parsing, help and validation all derive from it |
| `server.c` / `server.h` | HTTP server, OpenAI-compatible endpoints, SSE streaming |
| `chat.c` / `chat.h` | interactive REPL and its slash commands |
| `agent.c` / `agent.h` | shared prompt building and tool calling for all three modes |

## Model and inference

| file | role |
|---|---|
| `qwen35.c` | the engine: 24 blocks, attention + Gated DeltaNet, KV cache, recurrent state |
| `tokenizer.c` | byte-level BPE, matching llama.cpp's `qwen35` pre-tokeniser |
| `sampler.c` | repetition penalty, top-k, top-p, temperature; `-t 0` is argmax |
| `quant.c` | dequantisation for every supported weight format |
| `gguf.c` | GGUF container parsing; mmap; metadata |

## Compute backends

| file | role |
|---|---|
| `backend.c` / `backend.h` | backend table, CPU detection, activation quantisation, `q_matvec` row split |
| `backend_avx2.c` | AVX2 kernels. **The only object compiled `-mavx2`** |
| `backend_a2stub.c` | fallbacks for builds that do not link the AVX2 object; guarded by `INFER_A2_LINKED` |
| `backend_mmx.c` | MMX kernels in GNU inline asm; the only non-strict-C89 file on x86 |
| `backend_vis.c` | VIS 1 kernels for UltraSPARC; two compiler paths (Studio and GCC) |
| `tpool.c` / `tpool.h` | the thread pool: pthreads or Win32 events, spin-then-block barrier |

## Templates and tools

| file | role |
|---|---|
| `jinja.c`, `jinja_eval.c`, `jinja.h`, `jinja_priv.h` | Jinja2 subset sufficient for real GGUF chat templates |
| `mcp.c` / `mcp.h` | MCP client over Streamable HTTP |
| `json.c` / `json.h` | minimal JSON parser and writer |

## Platform and support

| file | role |
|---|---|
| `net_posix.c` / `net_win32.c` / `net.h` | sockets, one file per platform |
| `sys_posix.c` / `sys_win32.c` / `sys.h` | file mapping, time, CPU count |
| `prof.c` / `prof.h` | per-stage timers; compiled out without `-DINFER_PROFILE` |
| `util.c` | allocation wrappers, string buffer, logging |
| `infer.h` | version, shared types, byte-order detection |
| `unicode_tbl.h` | generated Unicode tables for the tokeniser |

## Tools

| file | role |
|---|---|
| `tools/gen_unicode.py` | regenerates `unicode_tbl.h` |
| `tools/ref_numpy.py` | independent NumPy reference for kernel output |
| `tools/mcp_test_server.py` | a local MCP server for testing tools |
| `tools/count_dynamic.py`, `tools/cross-count.sh` | instruction-mix counting for kernel work |

## Where to change what

| goal | file(s) |
|---|---|
| add a command-line option | one row in `opts.c` |
| change prompt or tool behaviour | `agent.c` |
| add a weight format | `quant.c` + every backend kernel + `t_ident` |
| add a compute backend | new `backend_<isa>.c`, one row in `backend.c`'s table, a gated object in the Makefile |
| change the HTTP surface | `server.c` |
| change sampling | `sampler.c` |
| change the architecture | `qwen35.c` |
| change threading | `tpool.c` — read [../06-PITFALLS.md](../06-PITFALLS.md) first |
