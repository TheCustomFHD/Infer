# infer

A GGUF inference server and chat client for the **Qwen3.5**
architecture, in **ANSI C (C89)**, with **no third-party
dependencies** — libc, libm and the OS socket API only.

CPU only. One binary per platform runs on an i486 and accelerates on
anything newer, chosen at run time.

```sh
make all                                   # four binaries
./infer-<version>-linux64 chat model.gguf  # go
```

---

## Where to look

Read in order if you are new. Jump straight in if you are not.

| # | document | answers |
|---|---|---|
| — | **this file** | what is it, is it for me, 60-second start |
| 1 | **[01-USING.md](01-USING.md)** | every command, flag and chat command |
| 2 | **[02-BUILDING.md](02-BUILDING.md)** | every target, flag, toolchain, verbatim recipes |
| 3 | **[03-ARCHITECTURE.md](03-ARCHITECTURE.md)** | how a token is produced, and why the code looks like this |
| 4 | **[04-PERFORMANCE.md](04-PERFORMANCE.md)** | real numbers, hard ceilings, how to optimise without fooling yourself |
| 5 | **[05-TESTING.md](05-TESTING.md)** | every test, what it proves, how to run it |
| 6 | **[06-PITFALLS.md](06-PITFALLS.md)** | every trap that has cost real time, and how to avoid it |
| 7 | **[07-AGENTS.md](07-AGENTS.md)** | the working loop for an AI agent on this repo |

Reference, for lookup rather than reading:

| file | contents |
|---|---|
| [reference/cli.md](reference/cli.md) | complete flag table, defaults, which modes accept what |
| [reference/files.md](reference/files.md) | one line per source file, and where to change what |
| [reference/findings.md](reference/findings.md) | index of all 55 measured findings |
| [reference/workspace.md](reference/workspace.md) | sandbox budget, cleanup, Wine, backups |

History lives in [`../CHANGELOG.md`](../CHANGELOG.md); the long-form
findings live in [`../docs/FINDINGS.md`](../docs/FINDINGS.md).

---

## What it does

- **`serve`** — OpenAI-compatible HTTP API. Existing clients work
  unchanged, including the official OpenAI SDK.
- **`chat`** — interactive multi-turn REPL with slash commands.
- **`run`** — one-shot generation on the console.
- **`info`** — dump model metadata and exit.
- **MCP tools** over Streamable HTTP.
- **Jinja chat templates** rendered from the GGUF itself, byte-identical
  to Python's Jinja2 on the templates tested.

## What it does not do

Stated plainly so you can rule it out fast:

- **Only the `qwen35` architecture.** Not llama, not mistral. It will
  say so and exit.
- **CPU only.** No CUDA, no Metal, no BLAS.
- **No HTTPS** — no TLS stack. Use a local endpoint or a proxy.
- **No stdio MCP** — HTTP only; stdio needs process spawning, which
  has no portable C89 form.
- **One request at a time.** The engine holds a single recurrent
  state, so `serve` serialises requests.
- **No bundled weights or chat templates.** Third-party content is not
  vendored.

## Supported targets

| platform | binary | notes |
|---|---|---|
| Linux x86 32-bit | `infer-<v>-linux` | runs on a 486 |
| Linux x86 64-bit | `infer-<v>-linux64` | |
| Windows x86 32-bit | `infer-<v>-windows.exe` | XP and later, Geode |
| Windows x86 64-bit | `infer-<v>-windows64.exe` | |
| Solaris / SPARC | `make solaris` | big-endian, VIS kernels |

All four x86 binaries are built at the **i486 baseline** and carry
every kernel — `avx2`, `mmx`, `i8`, `ref` — selected at run time by a
CPUID (and for AVX2, XGETBV) probe. There is no ISA download to choose
between and no way to pick wrong.

## 60-second start

```sh
# 1. build
make all

# 2. fetch a model (508 MiB / 532 MB on disk)
curl -L -o model.gguf \
  https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf

# 3. use it
./infer-<version>-linux64 run  model.gguf -p "whats the capital of germany"
./infer-<version>-linux64 chat model.gguf
./infer-<version>-linux64 serve model.gguf --port 8080
```

Threading is **opt-in**: add `-T 2` (or `-T 0` for one per core).
See [01-USING.md](01-USING.md#threads).

## Performance in one line

On a 2-core Xeon @2.60 GHz with the 0.8B Q4_K_M model: **~14 tok/s on
one core, ~22 tok/s on two** (Linux, 64-bit, AVX2). The workload is
memory-bound — it streams ~520 MB per generated token. Full numbers
and the reasoning in [04-PERFORMANCE.md](04-PERFORMANCE.md).

## Licence

MIT for the code. The **model** is Apache-2.0 from Alibaba; the GGUF
conversion is by Unsloth. No weights are included in this repository.
