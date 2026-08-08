# Using infer

Everything you can type at it. For the exhaustive flag table with
defaults see [reference/cli.md](reference/cli.md); this page is the
guided version.

- [Four commands](#four-commands)
- [Getting a model](#getting-a-model)
- [chat](#chat)
- [serve](#serve)
- [run](#run)
- [info](#info)
- [Sampling](#sampling)
- [Threads](#threads)
- [Backends and kernels](#backends-and-kernels)
- [Measuring](#measuring)
- [Templates](#templates)
- [MCP tools](#mcp-tools)
- [Troubleshooting](#troubleshooting)

---

## Four commands

```sh
infer serve <model.gguf> [options]   # OpenAI-compatible HTTP server
infer chat  <model.gguf> [options]   # interactive multi-turn chat
infer run   <model.gguf> [options]   # generate once on the console
infer info  <model.gguf>             # model metadata, then exit
```

**Options are shared between modes wherever they can be.** The same
`--system`, `--think`, `--raw`, `--template` and `--mcp` work in
`serve`, `chat` and `run` alike.

Using an option where it has no meaning is **reported, never silently
ignored** — passing `--host` to `run` tells you so. That is deliberate:
silently ignored input is a correctness bug, not a UX wrinkle.

## Getting a model

Only the **`qwen35`** architecture is implemented. Check with
`infer info model.gguf` — a mismatch prints
`architecture 'llama' is not supported` and exits.

```sh
curl -L -o model.gguf \
  https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf
```

508 MiB (532 MB on disk). The file is `mmap`ed, so start-up is
instant; see [04-PERFORMANCE.md](04-PERFORMANCE.md#memory) for what
that means for RSS.

## chat

```sh
infer chat model.gguf
infer chat model.gguf --system "You are terse." --think
infer chat model.gguf -p "hello"        # answer this, then continue
infer chat model.gguf --mcp http://127.0.0.1:3000/mcp
```

Keeps the whole conversation, re-renders it through the model's own
Jinja template every turn, and streams the reply.

### Slash commands

This is exactly what `/help` prints:

| command | effect |
|---|---|
| `/help` | this list |
| `/new`, `/clear` | start a fresh conversation |
| `/forget` | drop all but the system prompt |
| `/history` | show the conversation so far |
| `/system <text>` | set or replace the system prompt |
| `/think on\|off` | toggle the model's thinking block |
| `/raw on\|off` | bypass the chat template |
| `/tools` | list tools from the MCP server |
| `/prompt` | show the exact rendered prompt |
| `/stats` | tokens in the current conversation |
| `/perf` | speed of the session so far (needs `--log-perf`) |
| `/set` | show the options in effect, under their CLI names |
| `/quit`, `/exit` | leave |

`/prompt` is the debugging tool: it prints the literal string handed
to the model, which settles template arguments quickly.

## serve

```sh
infer serve model.gguf
infer serve model.gguf --host 0.0.0.0 --port 8080 --api-key secret
infer serve model.gguf --system "You are terse."
```

Speaks the OpenAI API:

```sh
curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local","messages":[{"role":"user","content":"hi"}]}'
```

- `GET /v1/models` — lists the model under `--alias`, or its filename.
- `POST /v1/chat/completions` — streaming and non-streaming.

**CLI flags are defaults; the request overrides them.** `--system` on
the command line applies to any request that does not carry its own
system message.

**One request at a time.** The engine holds a single recurrent state,
so requests are serialised and each resets that state.

**Auth is off unless you set `--api-key`.** With it, requests need
`Authorization: Bearer <key>`.

`--raw` makes `/v1/chat/completions` concatenate message contents
instead of rendering Jinja — for harnesses like llama-bench that
dislike templates.

## run

```sh
infer run model.gguf -p "Write a haiku about DOS"
infer run model.gguf -p "once upon a time" --raw
echo "explain gravity" | infer run model.gguf      # stdin if no -p
```

One prompt, one completion, exit. `-t 0` (greedy) makes output
deterministic, which is what every measurement and comparison in this
project uses.

## info

```sh
infer info model.gguf
```

Dumps GGUF metadata: architecture, hyper-parameters, tensor count,
tokeniser, quantisation. Reads headers only — no weights are loaded,
so it is instant.

## Sampling

| flag | default | effect |
|---|---|---|
| `-t, --temp <f>` | 0.7 | randomness; **0 = greedy/deterministic** |
| `--top-p <f>` | 0.8 | nucleus sampling |
| `--top-k <n>` | 40 | top-k sampling |
| `--repeat <f>` | 1.05 | repetition penalty |
| `--seed <n>` | from clock | fix for reproducible sampling |

For benchmarking or A/B testing always use `-t 0 --seed 1`: any other
setting makes two runs incomparable.

## Threads

**Single-threaded unless you ask.**

```sh
infer run model.gguf -p hi            # 1 thread
infer run model.gguf -p hi -T 4       # 4 threads
infer run model.gguf -p hi -T 0       # one per logical CPU
```

`-T`/`--threads` splits the **rows** of each matrix-vector product
across cores, and the Gated DeltaNet heads the same way. Rows are
independent, so **output is bit-identical at any thread count** — no
partial sum ever crosses a thread. `tests/t_thread.c` asserts this for
every backend by name.

Measured, 2-core Xeon, 0.8B Q4_K_M, 40 tokens greedy, Linux 64-bit:

| threads | tok/s |
|---|---|
| `-T 1` | ~12.8 |
| `-T 2` | ~18–22 |
| `-T 4` (oversubscribed) | no better |

**Asking for more threads than you have cores does not help.** The
pool detects it and stops spinning, so it costs nothing, but the work
is memory-bound and the extra threads have nothing to do.

Why opt-in: a default that quietly takes every core is wrong for the
hardware this project targets, and until 1.23.0 the pool's own barrier
made threading barely worthwhile anyway
(see [04-PERFORMANCE.md](04-PERFORMANCE.md#threading)).

## Backends and kernels

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

| backend | needs | notes |
|---|---|---|
| `avx2` | AVX2 **and** OS `ymm` support | fastest by far |
| `vis` | UltraSPARC VIS 1 | SPARC only; replaces `mmx` there |
| `mmx` | MMX | fastest without AVX2 |
| `i8` | 486 | portable integer C |
| `ref` | 486 | float reference, ground truth |

Chosen automatically; override with `--backend <name>`.

**A surprise worth knowing:** on a 64-bit build `i8` beats `mmx`
(3.5 vs 3.3 tok/s), because the compiler may assume SSE2 and
vectorises the portable C past the hand-written MMX. In the 32-bit
i486-baseline build `i8` stays scalar and `mmx` wins by 2x. `auto`
knows this.

### Per-format kernels

```sh
infer --kernel list           # current assignment
infer --kernel bench -v       # measure here, pick winners (no model needed)
infer --kernel q6k=i8         # pin one format
```

**Mixing is not numerically free.** `i8`, `mmx` and `vis` are
bit-identical to each other on every format. `avx2` deliberately is
not on the k-quants: it quantises the activation per 256 values
instead of per 32 and reassociates the sum. Measured against `i8` it
matches exactly on `Q8_0` and differs on `Q4_K`/`Q5_K`/`Q6_K`. Its
accuracy is bounded against the float reference by
`tests/t_avx2acc.c`. Pinning `avx2` for one format changes that
format's rounding; mixing the integer kernels does not.

**`--backend ref` is always the numerical ground truth.** If output
looks wrong, try it: if `ref` is right and a fast backend is not,
that is a bug worth reporting.

## Measuring

```sh
infer run model.gguf -p "..." -n 40 -t 0 --log-perf
```

```
--- perf: run ---
  prompt      :    18 tok in 907 ms    (19.855 tok/s, 0.05 s/tok)
  generation  :    24 tok in 1.78 s    (13.491 tok/s, 0.07 s/tok)
  TTFT        : 907 ms
  total       : 2.69 s
```

`--log-file <f>` writes to a file instead of stderr, flushed per line
so a killed run still leaves usable data.

### Per-stage profile

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

Needs a `-DINFER_PROFILE` build. **Every shipped binary has it**, so
the released downloads work as-is. A build without it says so rather
than printing nothing. There is no `make profile` target — it is a
flag: `make linux PROFILE=1`.

**Read `us/call` carefully.** Call sizes differ by 6x between formats,
so per-call time compares tensor shapes, not kernel quality. Normalise
by work — see [04-PERFORMANCE.md](04-PERFORMANCE.md).

## Templates

`infer` renders the model's own Jinja chat template from the GGUF. To
override:

```sh
infer chat model.gguf --template mytemplate.jinja
```

No template is bundled — third-party work is not vendored. A community
template can be fetched separately:

```sh
curl -L -o qwen-fixed.jinja \
  https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates/resolve/main/chat_template.jinja
infer chat model.gguf --template qwen-fixed.jinja
```

The engine implements a Jinja2 subset sufficient for real GGUF chat
templates, verified byte-identical against Python's Jinja2. Use
`/prompt` in chat to see exactly what a template produced.

## MCP tools

```sh
infer chat model.gguf --mcp http://127.0.0.1:3000/mcp
```

```
> What is the weather in Springfield?
  [tool] get_weather {"city":"Springfield"}
  [ -> ] Weather in Springfield: 14 C, light rain, wind 12 km/h.
It is 14 C and lightly raining in Springfield.
```

- **HTTP only** (Streamable HTTP transport). stdio is not supported.
- **No HTTPS** — no TLS stack.
- `--tool-rounds <n>` caps tool calls per reply (default 4).
- `--quiet-tools` hides the call/result lines.
- In `serve`, the tool loop runs server-side unless the request brings
  its own `"tools"` array.

**A 0.8B model is not reliable at tool use.** It will usually call the
right tool and may then mis-state the result. The plumbing is
verified; the model is small.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `architecture 'llama' is not supported` | this engine implements `qwen35` only; check `infer info` |
| Output is garbage | try `--backend ref`. If `ref` is right, it is a kernel bug. If `ref` is also wrong, suspect the model file |
| `prompt is longer than the server context window` | raise `-c`, or `/forget` in chat |
| Server accepts anything | set `--api-key` |
| `--log-stages` prints a note and nothing else | build without `-DINFER_PROFILE`; rebuild with `PROFILE=1` |
| Slower with `-T 8` than `-T 2` | oversubscription; the work is memory-bound. Use at most one thread per core |
| Windows: no output above `-T 1` | fixed in 1.23.2 — update. See [06-PITFALLS.md](06-PITFALLS.md) |
