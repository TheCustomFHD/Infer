# User guide

Everything you can do with `infer`, with worked examples.

New here? [COMPILE.md](COMPILE.md) builds it; this explains using it.

---

## Contents

1. [Getting a model](#1-getting-a-model)
2. [Four commands](#2-four-commands)
3. [`infer chat`](#3-infer-chat)
4. [`infer serve`](#4-infer-serve)
5. [`infer run`](#5-infer-run)
6. [`infer info`](#6-infer-info)
7. [Sampling](#7-sampling)
8. [Compute backends](#8-compute-backends)
9. [Threads](#9-threads)
10. [Custom templates](#10-custom-templates)
11. [MCP tools](#11-mcp-tools)
12. [Measuring performance](#12-measuring-performance)
13. [Troubleshooting](#13-troubleshooting)

---

## 1. Getting a model

Only the **`qwen35`** architecture is supported. Other GGUFs are
rejected with a clear message rather than producing noise.

```sh
curl -L -o Qwen3.5-0.8B-Q4_K_M.gguf \
  https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf
```

Check any file with `infer info model.gguf`.

---

## 2. Four commands

```sh
infer chat  model.gguf    # interactive conversation
infer serve model.gguf    # OpenAI-compatible HTTP server
infer run   model.gguf    # one-shot generation
infer info  model.gguf    # metadata
```

`infer model.gguf` alone means `serve`.
`infer --backend list` works with no model at all.

### Options are shared between modes

Every toggle works in every mode where it can possibly mean something.
`--system`, `--think`, `--raw`, `--template`, `--mcp` and all the
sampling knobs behave identically in `serve`, `chat` and `run` — they
are declared once, in [`src/opts.c`](docs/files/opts-c.md), which also
generates `--help`, so the two can never disagree.

Only four options are restricted, because nothing outside a listening
socket can use them: `--host`, `--port`, `--api-key` and `--alias`
(`serve` only), plus `-p/--prompt`, which `serve` takes from the request
body instead.

Using an option where it does not apply is an **error**, never a silent
no-op:

```
$ infer run model.gguf --port 9090
'--port' has no effect in run mode (it applies to: serve)
```

---

## 3. `infer chat`

```sh
infer chat model.gguf
infer chat model.gguf --system "You are terse." -c 2048
```

```
infer chat -- Qwen3.5-0.8B
context 2048 tokens, backend mmx. /help for commands, /quit to exit.

> My name is Ada.
Hello, Ada! How can I help you today?

> What is my name?
My name is **Ada**. 😊
```

### Commands

| command | effect |
|---|---|
| `/help` | list commands |
| `/new`, `/clear` | fresh conversation |
| `/forget` | drop history, keep the system prompt |
| `/history` | show the conversation |
| `/system <text>` | set or replace the system prompt |
| `/think on\|off` | toggle the model's reasoning block |
| `/raw on\|off` | bypass the chat template mid-session |
| `/tools` | list MCP tools |
| `/prompt` | print the exact rendered prompt |
| `/stats` | messages and prompt tokens vs context |
| `/perf` | speed of the session so far (needs `--log-perf`) |
| `/set` | show the options in effect, under their command-line names |
| `/quit`, `/exit` | leave |

### Options

These are the general options; all of them work in `serve` and `run`
too. See `infer --help` for the complete list.

```
--system <text>    system prompt
--think [on|off]   emit the <think> block; bare --think means on,
                   --think off is the explicit default
--raw              no chat template at all
--template <file>  use a custom Jinja template
--mcp <url>        MCP server over HTTP
--tool-rounds <n>  max tool calls per reply (default 4)
--quiet-tools      do not print tool calls and results
-c, --ctx <n>      context window (default 4096)
-n, --predict <n>  max tokens per reply (default 512)
-p, --prompt <t>   answer this first, then continue interactively
```

`-p` seeds the opening turn and then hands over to the prompt, which is
handy for a scripted opener:

```sh
infer chat model.gguf -p "Summarise the file I paste next."
```

`/set` prints the options currently in effect, and `/think on|off` and
`/raw on|off` flip them mid-conversation.

**On slow machines:** every turn re-ingests the whole conversation, so
prompt cost grows with history. `/stats` shows what you are carrying;
`/forget` resets it. Starting with `-c 1024` keeps turns brisk.

---

## 4. `infer serve`

```sh
infer serve model.gguf
infer serve model.gguf --host 0.0.0.0 --port 8080 --api-key secret
```

| endpoint | notes |
|---|---|
| `POST /v1/chat/completions` | streaming and non-streaming |
| `POST /v1/completions` | raw text completion |
| `GET /v1/models` | model list |
| `GET /health` | liveness |

```sh
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello"}]}'
```

Works with the official SDK:

```python
from openai import OpenAI
c = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="none")
print(c.chat.completions.create(
    model="local",
    messages=[{"role":"user","content":"Name one planet."}]
).choices[0].message.content)
```

Honoured fields: `messages`, `prompt`, `max_tokens`,
`max_completion_tokens`, `temperature`, `top_p`, `top_k`, `seed`,
`stop`, `stream`, `repeat_penalty`, `frequency_penalty`,
`enable_thinking`, `think`, `tools`, `raw`.

### CLI defaults, per-request overrides

`serve` accepts the same prompting options as the other modes and uses
them as the default for every request. Anything a request states
explicitly wins:

| option | effect in `serve` | overridden by |
|---|---|---|
| `--system <text>` | inserted when the request has no system message | a `system` message in `messages` |
| `--think [on\|off]` | default for `enable_thinking` | `"enable_thinking"` or `"think"` in the body |
| `--raw` | chat requests bypass the template | `"raw": false` in the body |
| `--mcp <url>` | server runs the tool loop itself | a `"tools"` array in the body |
| `--template <file>` | replaces the GGUF template | — |

```sh
# every request gets this persona unless it brings its own
infer serve model.gguf --system "You are a terse assistant."
```

### `--raw` for benchmark harnesses

Some clients — `llama-bench` among them — dislike Jinja templates.
`--raw` makes `/v1/chat/completions` concatenate the message contents
and feed them to the model verbatim:

```sh
infer serve model.gguf --raw
```

A single request can opt back in with `"raw": false`.

### Tools without a tool-aware client

With `--mcp`, the server discovers the tools, notices when the model
asks for one, calls it, feeds the result back and repeats — up to
`--tool-rounds` times. The client sees only the final answer, so a
plain OpenAI client gets tool use for free:

```sh
infer serve model.gguf --mcp http://127.0.0.1:3000/mcp
```

If a request carries its own `"tools"` array, the client is driving
instead: those tools are rendered into the prompt and the tool calls
come back for the client to execute.

Without `--api-key` the server accepts any caller — keep the default
`127.0.0.1` bind unless you mean otherwise.

**One request at a time.** The engine holds a single recurrent state, so
requests are serialised.

---

## 5. `infer run`

One-shot, no conversation:

```sh
infer run model.gguf -p "Write a haiku about DOS"
infer run model.gguf -p "..." -n 100 -t 0
echo "Explain gravity." | infer run model.gguf
```

`run` is not a reduced mode: it takes the same options as `chat`, and
renders prompts through the same template code, so these all work.

```sh
infer run model.gguf -p "What fruit?" --system "Answer in one word."
infer run model.gguf -p "Weather in Springfield?" --mcp http://127.0.0.1:3000/mcp
infer run model.gguf -p "..." --template qwen-fixed.jinja
infer run model.gguf -p "..." --think
```

With `--mcp`, `run` runs the whole tool loop and prints the final
answer, so a single command can call a tool and use the result.

`--raw` skips the chat wrapper and feeds the text verbatim — useful for
base models or for testing a broken template.

`-t 0` is greedy and therefore deterministic. Good for comparing builds.

---

## 6. `infer info`

```sh
infer info model.gguf
```

Prints architecture, tensor count, blob size and every metadata key
(abbreviating the 248k-entry tokeniser arrays).

---

## 7. Sampling

| flag | default | effect |
|---|---|---|
| `-t, --temp` | 0.7 | 0 = greedy/deterministic; higher = more varied |
| `--top-p` | 0.8 | nucleus sampling |
| `--top-k` | 40 | consider only the top k tokens |
| `--repeat` | 1.05 | penalise recent tokens |
| `--seed` | clock | fix for reproducible output |

```sh
infer run model.gguf -p "..." -t 0                    # deterministic
infer run model.gguf -p "..." -t 1.0 --top-p 0.95     # creative
infer run model.gguf -p "..." -t 0.7 --seed 42        # reproducible
```

---

## 8. Compute backends

```sh
infer --backend list
```

```
CPU: GenuineIntel  features: mmx cmov

available backends:
  avx2  ok  integer kernels with AVX2 VPMADDUBSW inner loop (Haswell+)   <- selected
  mmx   ok  integer kernels with MMX inner loop (Pentium MMX / K6 / Geode+)
  i8    ok  integer kernels, portable C (recommended)
  ref   ok  scalar float reference, maximum portability (486-safe)
```

| backend | requires | notes |
|---|---|---|
| `avx2` | AVX2 **and** OS `ymm` support (Haswell 2013+, Windows 7 SP1+) | fastest by far |
| `mmx` | MMX (Pentium MMX, K6, Geode LX+) | fastest without AVX2 |
| `i8` | 486 | portable integer path |
| `ref` | 486 | reference / ground truth |

Chosen automatically after a CPUID probe. Override with
`--backend avx2|mmx|i8|ref`.

**All four are in every binary**, including the 32-bit one. Only the
AVX2 kernel is compiled with AVX2 instructions, in its own object,
behind a runtime gate — so the same executable starts on a 486 and
uses AVX2 on a modern chip.

The AVX2 gate checks CPUID **and** `XGETBV`. Windows XP never saved
the `ymm` registers across a context switch, so an XP machine on
Haswell silicon reports AVX2 in CPUID and cannot execute it; checking
only CPUID would crash instead of falling back to `mmx`.

One result that surprises people: **on a modern 64-bit build `i8` is
faster than `mmx`.** With SSE2 guaranteed, the compiler vectorises the
portable `i8` loops past the hand-written 64-bit MMX kernel, and
`auto` prefers it. In the 32-bit build, compiled to an i486 baseline,
`i8` stays scalar and `mmx` wins. Both are right for their target.

**If output ever looks wrong, `--backend ref` is the numerical ground
truth.** `avx2`, `mmx`, `i8` and `ref` agree bit-for-bit on the
k-quant formats, except the AVX2 k-quant kernels, which are
deliberately a different (equally valid) rounding — bounded against
`ref` by `tests/t_avx2acc.c`.

---

## 9. Threads

**`infer` runs single-threaded unless you ask otherwise.**

```sh
infer run model.gguf -p "hi"            # 1 thread (default)
infer run model.gguf -p "hi" -T 4       # 4 threads
infer run model.gguf -p "hi" -T 0       # one thread per logical CPU
```

`--threads` / `-T` splits the rows of each matrix-vector product
across cores. Rows are independent, so **the result is bit-identical
at any thread count** — no partial sum ever crosses a thread.

It stays opt-in because a default that quietly uses every core is the
wrong default for the hardware this project targets — but since
1.23.0 it is worth switching on. Measured on a 2-core Xeon,
Qwen3.5-0.8B-Q4_K_M, 40 tokens greedy:

| threads | 1.22.0 | 1.23.0 |
|---|---|---|
| `-T 1` | 13.99 tok/s | 14.12 tok/s |
| `-T 2` | 17.25 tok/s | **22.16 tok/s** |

Before 1.23.0 the thread pool woke and slept its workers through a
condition variable on every one of the ~95 parallel regions per token,
costing about 44 us each; the process sat at 159% CPU on two cores
instead of ~200%. It now spins briefly before blocking.

**Asking for more threads than you have cores does not help.** The
pool detects that case and goes back to blocking, so it costs nothing,
but the work is memory-bound and extra threads have nothing to do:

```
-T 2   22.2 tok/s      (2 physical cores)
-T 4   18.2 tok/s
-T 8   17.2 tok/s
```

More cores help most when memory bandwidth is not the limit. This
workload streams the whole model once per token — about 520 MB — so
the ceiling is your RAM, not your CPU. Measure with `--log-perf`
rather than assuming.

---

## 10. Custom templates

```sh
infer chat  model.gguf --template mytemplate.jinja
infer serve model.gguf --template mytemplate.jinja
```

See [docs/TEMPLATES.md](docs/TEMPLATES.md) for supported Jinja and how to
debug a template with `/prompt`.

---

## 11. MCP tools

```sh
python3 tools/mcp_test_server.py 3000          # a test server
infer chat model.gguf --mcp http://127.0.0.1:3000/mcp
```

```
connecting to MCP server http://127.0.0.1:3000/mcp ...
  test-mcp: 2 tools
    get_weather              Get the current weather for a city
    add                      Add two numbers

> What is the weather in Springfield?
  [tool] get_weather {"city":"Springfield"}
  [ -> ] Weather in Springfield: 14 C, light rain, wind 12 km/h.
It is 14 C and lightly raining in Springfield.
```

**HTTP only** (Streamable HTTP transport). stdio is not supported —
it needs child-process spawning, which has no portable ANSI C form.
**https is not supported** — no TLS stack; use a local endpoint or a
proxy.

**A 0.8B model is not reliable at tool use.** It will usually call the
right tool and may then mis-state the result. The plumbing is verified;
the model is small.

---

## 12. Measuring performance

```sh
infer run model.gguf -p "..." -n 10 -t 0 --log-perf
```

```
--- perf: run ---
  prompt      :    19 tok in 3.80 s    (4.995 tok/s, 0.20 s/tok)
  generation  :     8 tok in 3.01 s    (2.660 tok/s, 0.38 s/tok)
  TTFT        : 3.80 s
  total       : 6.81 s
```

`--log-file perf.txt` writes to a file instead of stderr.

### Per-stage breakdown

`--log-stages` prints where the time went inside each forward pass:

```sh
infer run model.gguf -p "..." -n 40 -t 0 --log-stages
```

```
stage                       seconds       %     calls  us/call
TOTAL forward pass            3.638  100.0%        58  62718.2
  embedding lookup            0.000    0.0%        58      5.1
  attention layers (6)        0.227    6.3%       348    653.3
  delta-net layers (18)       1.179   32.4%      1044   1128.9
    input projections         0.714   19.6%      1044    684.2
    causal conv + SiLU        0.071    2.0%      1044     68.2
    state recurrence          0.173    4.7%      1044    165.4
    output projection         0.214    5.9%      1044    205.0
  feed-forward (24)           1.229   33.8%      1392    882.9
  rms norms                   0.006    0.2%      2784      2.1
  lm head                     0.995   27.4%        41  24277.7
```

This needs a build with `-DINFER_PROFILE`. **Every shipped binary has
it**, so the released downloads work as-is; a build you made with
plain `make linux` also has it only if you passed `PROFILE=1`. A build
without it prints a note saying so rather than silently showing
nothing. There is no `make profile` target — it is a flag:
`make linux PROFILE=1`. See [PROFILING.md](PROFILING.md).

### Choosing kernels per weight format

`--kernel` pins a specific backend to one weight format, and
`--kernel bench` measures every combination on this machine and picks
the winners:

```sh
infer --kernel list             # show the current assignment
infer --kernel bench -v         # time them all here, no model needed
infer --kernel q6k=i8 run model.gguf -p "..."
```

Mixing the integer kernels (`i8`, `mmx`, `vis`) is purely a speed
choice — they are bit-identical. Pinning `avx2` for a k-quant format
changes that format's rounding slightly; see section 8.

### Other diagnostics

```sh
infer --version                 # version string, then exit
infer -v run model.gguf -p hi   # --verbose: load progress, chosen backend,
                                # thread count, and each request in serve
```

Reference numbers:

| machine | s/token |
|---|---|
| Intel i7-9750H | ~0.4 |
| AMD Geode LX 800 @ 500 MHz | ~15.8 |

---

## 13. Troubleshooting

**`architecture 'llama' is not supported`** — this engine implements
`qwen35` only. Check with `infer info`.

**Output is garbage** — try `--backend ref`. If `ref` is correct and a
fast backend is not, that is a bug worth reporting. If `ref` is also
wrong, suspect the model file.

**`prompt is longer than the server context window`** — raise `-c`, or
`/forget` in chat.

**Server accepts anything** — set `--api-key`.

**MCP connection fails** — https is unsupported; check the URL is
`http://` and that the path matches (often `/mcp`).

**Model loads but output is nonsense after a few turns** — check
`/prompt`. A wrong template produces plausible-looking but subtly broken
prompts.

**Very slow on old hardware** — expected. See
[docs/PERFORMANCE-ANALYSIS.md](docs/PERFORMANCE-ANALYSIS.md) for why,
and what actually helps (a smaller model, mostly).

---

## See also

- [COMPILE.md](COMPILE.md) · [CHAT.md](CHAT.md) ·
  [docs/TEMPLATES.md](docs/TEMPLATES.md) ·
  [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
