# infer

A self-contained GGUF inference server and chat client for **Qwen3.5**,
written in **ANSI C (C89)** with **no dependencies** beyond the C
standard library and the operating system's own socket calls.

CPU only. No SSE, no threads, no BLAS, no `ggml`, no `libcurl`, no JSON
library. The target is a **486**.

![infer running](imgs/example.jpg)

```
$ infer chat model.gguf
infer chat -- Qwen3.5-0.8B
context 4096 tokens, backend mmx. /help for commands, /quit to exit.

> My name is Ada.
Hello, Ada! How can I help you today?

> What is my name?
My name is **Ada**. 😊
```

---

## What it does

| | |
|---|---|
| **Runs Qwen3.5** | a hybrid architecture — 6 attention layers, 18 Gated DeltaNet layers |
| **OpenAI API** | `/v1/chat/completions` with streaming, `/v1/completions`, `/v1/models` |
| **Interactive chat** | multi-turn, with history commands |
| **Real Jinja templates** | renders the GGUF's own template, or any `.jinja` file |
| **Byte-order neutral** | bit-identical output on big-endian hosts (verified on s390x and SPARC) |
| **MCP tools** | Model Context Protocol over HTTP, in every mode |
| **One set of options** | `--system`, `--think`, `--raw`, `--mcp`, `--template` work the same in `serve`, `chat` and `run` |
| **Runs on a 486** | and on Windows XP, from the same sources |

---

## Quick start

```sh
# build
make

# get the model (532 MB)
curl -L -o Qwen3.5-0.8B-Q4_K_M.gguf \
  https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf

# chat
./infer chat Qwen3.5-0.8B-Q4_K_M.gguf

# or serve
./infer serve Qwen3.5-0.8B-Q4_K_M.gguf
```

Nothing to `pip install`, no CMake, no submodules.

---

## Documentation

| document | what it covers |
|---|---|
| **[USER-GUIDE.md](USER-GUIDE.md)** | every command and flag, with examples |
| **[AGENTS.md](AGENTS.md)** | guidance for AI coding agents; `.agents/` has the detail |
| **[COMPILE.md](COMPILE.md)** | atomic build steps for every target and toggle |
| **[CHAT.md](CHAT.md)** | the chat REPL and MCP tools |
| **[PROFILING.md](PROFILING.md)** | `--log-perf`, `--log-stages`, and reading the output |
| **[CHANGELOG.md](CHANGELOG.md)** | what changed, and why |
| **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** | how the pieces fit; data flow |
| **[docs/FINDINGS.md](docs/FINDINGS.md)** | 24 surprises, each with measurements |
| **[docs/PERFORMANCE-ANALYSIS.md](docs/PERFORMANCE-ANALYSIS.md)** | why the kernels are at their hardware floor |
| **[docs/TEMPLATES.md](docs/TEMPLATES.md)** | custom Jinja templates |
| **[docs/files/](docs/files/)** | one document per source file |
| **[docs/files/opts-c.md](docs/files/opts-c.md)** | how one option table drives parsing, help and validation |
| **[docs/files/agent-c.md](docs/files/agent-c.md)** | shared prompt building and tool calling |

---

## Commands

```sh
infer chat  <model.gguf>    # interactive multi-turn chat
infer serve <model.gguf>    # OpenAI-compatible HTTP server
infer run   <model.gguf>    # one-shot generation
infer info  <model.gguf>    # model metadata
infer --backend list        # CPU features and available kernels
```

Options are shared between modes wherever they can be, so the same flags
mean the same thing everywhere:

```sh
infer run   model.gguf -p "What fruit?" --system "One word only."
infer serve model.gguf --system "One word only." --think
infer serve model.gguf --raw                    # for llama-bench etc.
infer chat  model.gguf -p "hello" --mcp http://127.0.0.1:3000/mcp
```

An option used where it cannot apply is reported, never silently
dropped:

```
$ infer run model.gguf --port 9090
'--port' has no effect in run mode (it applies to: serve)
```

---

## Build targets

Three targets. Everything else is a flag.

```sh
make linux              # Linux/x86    -> infer-<version>-linux
make windows            # Windows/x86  -> infer-<version>-windows.exe
make solaris            # Solaris/SPARC-> infer-<version>-solaris
make all                # all six shipping artifacts

make test               # the test suite
```

**Every accelerated kernel is compiled in and chosen at run time** after
a CPUID / feature probe. The x86 binaries carry `mmx`, `i8` and `ref`; the Solaris binary
carries `vis`, `i8` and `ref`. One
binary per platform: `make linux` runs on a 486 and accelerates on
anything newer.

Runtime selection costs one indirect call per matrix *row* — measured at
the noise floor on both x86-64 and an i486-targeted build.

### Flags

| flag | effect |
|---|---|
| `PROFILE=1` | per-stage timers; names the binary `...-profile` |
| `NATIVE=1` | `-march=native`; **only runs on the build machine** |
| `BITS=64` | 64-bit Linux build, named `...-linux64` (default 32-bit) |
| `TOOLCHAIN=gcc` | build Solaris with GCC instead of Sun Studio |
| `NO_MMX=1` `NO_VIS=1` | leave a kernel out |
| `NO_SSE2=1` `NO_AVX2=1` | reserved for the dedicated SSE2/AVX2 kernels (not yet written) |

```sh
make linux PROFILE=1            # timers
make linux BITS=64 NATIVE=1     # fastest on this machine only
make solaris TOOLCHAIN=gcc      # GCC instead of Sun Studio
```

`make all` builds the six shipping artifacts — 32-bit and 64-bit Linux
and 32-bit Windows, each with and without timers. `NATIVE=1` is
excluded on purpose: it only runs on the machine that built it.

Which backend each one picks is decided at run time:

| artifact | picks | why |
|---|---|---|
| `-linux`, `-windows.exe` | `mmx` | i486 baseline, no SSE2 available |
| `-linux64` | `i8` | x86-64 implies SSE2, so the compiler vectorises `i8` past the hand-written MMX kernel |

The profiler stays a **compile-time** choice: its timers sit inside the
per-layer path, so a runtime test would tax every build, including the
slow machines this project exists for.

Solaris/SPARC is deliberately separate: different compiler, different
flags, big-endian, 64-bit.

Every target compiles with **zero warnings**. The i486 build was
disassembled and audited: **no SSE, MMX, CMOV or any post-486
instruction**. The Windows binaries import exactly three DLLs —
KERNEL32, msvcrt, WS2_32 — all present on a stock XP install.

Full detail in [COMPILE.md](COMPILE.md).

---

## Layout

The tree is split so that porting touches as little as possible. **Only
four files know an operating system exists.**

```
src/
  main.c          launcher: dispatch, the one-shot run loop
  opts.c          the single option table: parsing, --help, validation
  agent.c         messages -> prompt, tool calls (shared by all modes)
  server.c        HTTP/1.1, SSE, the OpenAI API
  chat.c          interactive REPL
  qwen35.c        the engine: the qwen35 architecture
  tokenizer.c     byte-level BPE
  jinja.c         Jinja2 subset: values
  jinja_eval.c    Jinja2 subset: parser and interpreter
  mcp.c           MCP client over HTTP
  backend.c       CPU detection, backend selection, i8 kernels
  backend_mmx.c   MMX kernels (optional)
  quant.c         Q4_K/Q5_K/Q6_K/Q8_0 decoders + the float reference
  gguf.c          GGUF container reader
  sampler.c       temperature / top-k / top-p / repeat penalty
  json.c          minimal JSON parser
  prof.c          profiling and throughput logging
  util.c          allocation, growable strings, logging

  net.h           >>> the networking interface <<<
  net_posix.c       BSD sockets
  net_win32.c       Winsock 2 (XP-safe)

  sys.h           >>> file mapping and time <<<
  sys_posix.c       mmap, gettimeofday
  sys_win32.c       CreateFileMapping, QueryPerformanceCounter
```

`server.c`, `chat.c` and `mcp.c` include `net.h` and nothing else, and
are byte-for-byte identical on both platforms.

---

## The model

`Qwen3.5-0.8B` is **not a plain transformer**, which is why a generic
llama.cpp-style loop will not run it:

```
24 blocks, n_embd 1024, FFN 3584

  blocks 3,7,11,15,19,23   gated grouped-query attention
                           8 query heads, 2 KV heads, head_dim 256
                           per-head Q/K RMSNorm, interleaved mRoPE,
                           output gated by a sigmoid

  the other 18             Gated DeltaNet (linear attention)
                           128x128 recurrent state per head,
                           depthwise causal conv, L2-normalised Q/K
```

DeltaNet layers cost the same per token regardless of context length —
only the six attention layers grow.

Validated against an independent NumPy implementation written from the
reference graph: both produce identical ranked logits.

---

## Performance

Single-threaded by design.

| machine | s/token |
|---|---|
| Intel i7-9750H | ~0.4 |
| AMD Geode LX 800 @ 500 MHz | ~15.8 |

Both compute kernels are within ~10% of what the hardware allows —
`mmx` at 8.9 cycles/MAC, `i8` at 21.8 against a modelled floor of ~22.
[docs/PERFORMANCE-ANALYSIS.md](docs/PERFORMANCE-ANALYSIS.md) documents
the measurements and every rejected optimisation.

Memory: the model is `mmap`ed, so the 0.8B model idles at **~44 MB RSS**
and starts instantly.

---

## Correctness

* **2,244 comparisons** — every tensor × 6 activation patterns × both
  fast backends against the scalar reference. Zero failures.
* **Jinja output byte-compared against Python Jinja2** — the built-in
  template and a 16 kB community template, with and without tools,
  thinking on and off. Byte-identical.
* Tokeniser round-trips CJK, emoji, accents, contractions and whitespace
  runs.
* Verified against the official OpenAI Python SDK.

`--backend ref` is always available as numerical ground truth.

---

## Licence

MIT for the code. The **model** is Apache-2.0 from Alibaba; the GGUF
conversion is by Unsloth. No weights are included here.
