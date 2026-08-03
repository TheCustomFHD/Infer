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
9. [Custom templates](#9-custom-templates)
10. [MCP tools](#10-mcp-tools)
11. [Measuring performance](#11-measuring-performance)
12. [Troubleshooting](#12-troubleshooting)

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
| `/tools` | list MCP tools |
| `/prompt` | print the exact rendered prompt |
| `/stats` | messages and prompt tokens vs context |
| `/perf` | speed of the session so far (needs `--log-perf`) |
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
CPU: AuthenticAMD  features: mmx 3dnow cmov

available backends:
  mmx   ok  integer kernels with MMX inner loop      <- selected
  i8    ok  integer kernels, portable C
  ref   ok  scalar float reference, 486-safe
```

| backend | requires | speed |
|---|---|---|
| `mmx` | MMX (Pentium MMX, K6, Geode LX+) | fastest |
| `i8` | 486 | ~2× `ref` |
| `ref` | 486 | reference / ground truth |

Chosen automatically by CPUID. Override with `--backend mmx|i8|ref`.

**If output ever looks wrong, `--backend ref` is the numerical ground
truth.** 2,244 automated comparisons across every tensor currently show
zero discrepancies, but that is the check to run.

---

## 9. Custom templates

```sh
infer chat  model.gguf --template mytemplate.jinja
infer serve model.gguf --template mytemplate.jinja
```

See [docs/TEMPLATES.md](docs/TEMPLATES.md) for supported Jinja and how to
debug a template with `/prompt`.

---

## 10. MCP tools

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

## 11. Measuring performance

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

`--log-file perf.txt` writes to a file. For a per-stage breakdown build
`make profile` (or `profile-geode`) — see [PROFILING.md](PROFILING.md).

Reference numbers:

| machine | s/token |
|---|---|
| Intel i7-9750H | ~0.4 |
| AMD Geode LX 800 @ 500 MHz | ~15.8 |

---

## 12. Troubleshooting

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
