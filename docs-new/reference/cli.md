# CLI reference

Generated from the option table in `src/opts.c`, which is the single
source of truth for parsing, `--help` and validation. If a flag is
here it exists; if it is not, it does not.

For the guided version with examples see [../01-USING.md](../01-USING.md).

## Commands

| command | purpose |
|---|---|
| `infer serve <model.gguf>` | OpenAI-compatible HTTP server |
| `infer chat <model.gguf>` | interactive multi-turn chat |
| `infer run <model.gguf>` | generate once on the console |
| `infer info <model.gguf>` | model metadata, then exit |

## All options

Using an option in a mode that does not accept it is **reported, not
ignored**.

| option | argument | modes |
|---|---|---|
| `--host` | <addr> | serve |
| `--port` | <n> | serve |
| `--api-key` | <key> | serve |
| `--alias` | <name> | serve |
| `--ctx` / `-c` | <n> | serve, chat, run |
| `--predict` / `-n` | <n> | serve, chat, run |
| `--temp` / `-t` | <f> | serve, chat, run |
| `--top-p` | <f> | serve, chat, run |
| `--top-k` | <n> | serve, chat, run |
| `--repeat` | <f> | serve, chat, run |
| `--seed` | <n> | serve, chat, run |
| `--system` | <text> | serve, chat, run |
| `--think` | [on|off] | serve, chat, run |
| `--raw` | — | serve, chat, run |
| `--template` | <file> | serve, chat, run |
| `--mcp` | <url> | serve, chat, run |
| `--tool-rounds` | <n> | serve, chat, run |
| `--quiet-tools` | — | serve, chat, run |
| `--backend` | <n> | all |
| `--log-perf` | — | serve, chat, run |
| `--log-stages` | — | serve, chat, run |
| `--log-file` | <file> | all |
| `--kernel` | <f=b> | all |
| `--threads` / `-T` | <n> | all |
| `--verbose` / `-v` | — | all |
| `--help` / `-h` | — | all |
| `--version` | — | all |

## Defaults

| option | default |
|---|---|
| `--host` | 127.0.0.1 |
| `--port` | 8080 |
| `--api-key` | none (no auth) |
| `--alias` | from the model file |
| `--ctx` / `-c` | 4096 |
| `--predict` / `-n` | 512 |
| `--temp` / `-t` | 0.7 (**0 = greedy**) |
| `--top-p` | 0.8 |
| `--top-k` | 40 |
| `--repeat` | 1.05 |
| `--seed` | from the clock |
| `--think` | off |
| `--tool-rounds` | 4 |
| `--threads` / `-T` | 1 (**0 = one per core**) |
| `--backend` | auto |

## Special argument values

| invocation | effect |
|---|---|
| `--backend list` | print backends and which one is selected |
| `--kernel list` | print the per-format kernel assignment |
| `--kernel bench` | measure every kernel here and pin the winners (no model needed) |
| `--kernel q6k=i8` | pin one format to one backend |
| `--think` alone | same as `--think on` |
| `--think off` | disable (the default) |
| `-T 0` | one thread per logical CPU |

## Chat slash commands

Exactly what `/help` prints.

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

## HTTP endpoints

| method | path | notes |
|---|---|---|
| `GET` | `/v1/models` | reports `--alias`, or the filename |
| `POST` | `/v1/chat/completions` | streaming and non-streaming |

Auth is off unless `--api-key` is set; with it, requests need
`Authorization: Bearer <key>`.
