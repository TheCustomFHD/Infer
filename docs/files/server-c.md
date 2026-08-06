# `src/server.c` — HTTP and the OpenAI API

**~1080 lines total.** HTTP/1.1 server exposing an OpenAI-compatible API.
Contains **no OS networking calls** — everything goes through
[net-h.md](net-h.md), which is why this file is byte-for-byte identical
on Linux and Windows XP.

## Endpoints

| method | path | notes |
|---|---|---|
| `POST` | `/v1/chat/completions` | streaming and non-streaming |
| `POST` | `/v1/completions` | raw text completion |
| `GET` | `/v1/models` | model list |
| `GET` | `/health` | liveness |
| `OPTIONS` | any | CORS preflight |

Honoured fields: `messages`, `prompt`, `max_tokens`,
`max_completion_tokens`, `temperature`, `top_p`, `top_k`, `seed`, `stop`
(string or array), `stream`, `repeat_penalty`, `frequency_penalty`,
`enable_thinking`.

## Prompt building

As of 1.7.0 the server renders the **model's own Jinja chat template**
(or a `--template` override) via [jinja-c.md](jinja-c.md), exactly as
`infer chat` does. Earlier versions hand-wrote the ChatML layout, which
was correct for Qwen3.5 and wrong for anything else.

OpenAI allows `content` to be a string *or* an array of
`{type:"text",text:...}` parts. The two paths handle that differently,
which is deliberate:

- **Templated** (the normal path): `messages_to_jinja()` converts the
  JSON to Jinja values with `agent_js_to_jj()`, preserving structure —
  an array stays a list, so a template that iterates `message.content`
  behaves as its author intended. Missing `role` defaults to `"user"`,
  missing `content` to `""`, and a non-object entry is skipped rather
  than crashing.
- **`--raw`**: `raw_from_messages()` walks the JSON directly and
  concatenates, joining messages with a blank line and pulling `text`
  out of each part of an array. That is what benchmark harnesses want.

## One request at a time

The engine holds a single recurrent state. Requests are serialised, and
each one resets that state and re-ingests its prompt. On the target
hardware concurrency would only make every client slower.

## UTF-8 in SSE streams

Byte-level BPE splits multi-byte characters across tokens. Emitting a
fragment produces invalid UTF-8 inside a JSON string, which strict
clients — including the official OpenAI SDK — reject outright.

`utf8_complete_prefix()` returns the length of the longest complete
prefix; anything trailing is held back until its remaining bytes arrive.
The non-streaming path applies the same guard, since generation can be
cut mid-character by `max_tokens`.

This was caught by testing against the real SDK, not by inspection.

## HTTP parsing

Deliberately minimal: request line, case-insensitive header lookup for
`Content-Length` and `Authorization`, then the body. 64 KB header cap,
4 MB body cap, 30 s timeouts. No chunked request bodies — no OpenAI
client sends them.

## CLI defaults and per-request overrides

`serve` takes the same prompting options as the other modes and treats
them as defaults; anything the request states explicitly wins.

| option | default for | overridden by |
|---|---|---|
| `--system` | requests with no system message | a `system` message |
| `--think` | `enable_thinking` | `"enable_thinking"` / `"think"` |
| `--raw` | template bypass | `"raw": false` |
| `--mcp` | server-side tool loop | a `"tools"` array |

### `--raw`

`/v1/chat/completions` concatenates message contents instead of
rendering Jinja. Some clients (`llama-bench`) dislike templates; this
gives them a plain continuation endpoint with the chat URL.

### The server-side tool loop

With `--mcp` and no client-supplied `tools`, `generate()` runs with a
`capture` buffer instead of the socket: the reply is inspected, any tool
call is executed, the result is appended as a `tool` message, and the
loop repeats up to `--tool-rounds`. Only the final answer is sent, so an
ordinary OpenAI client gets tool use without knowing it.

If the request carries its own `tools`, the client is driving: those are
rendered into the prompt and the tool calls are returned to it.

## Messages are converted whole

Earlier versions copied only `role` and `content` out of each incoming
message. Templates also read `message.tool_calls` and
`message.reasoning_content`, so replaying a tool-using conversation
through the API silently dropped its tool calls.
`agent_js_to_jj()` now converts the entire object.

## See also

- [net-h.md](net-h.md) · [json-c.md](json-c.md) · [jinja-c.md](jinja-c.md)
