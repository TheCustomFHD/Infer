# `infer chat` — interactive multi-turn chat

```sh
infer chat model.gguf
infer chat model.gguf --system "You are terse." --think
infer chat model.gguf --mcp http://127.0.0.1:3000/mcp
```

Keeps the whole conversation, renders it through the **model's own Jinja
chat template** on every turn, and streams the reply.

---

## Options

```
--system <text>   system prompt
--think           let the model emit its <think> reasoning block
--mcp <url>       MCP server over HTTP (Streamable HTTP transport)
-c, --ctx <n>     context window     (default 4096)
-n, --predict <n> max tokens per reply
-t, --temp <f>    temperature
--backend <n>     avx2 | mmx | i8 | ref
```

## Commands

| command | effect |
|---|---|
| `/help` | list commands |
| `/new`, `/clear` | start a fresh conversation |
| `/forget` | drop history, keep the system prompt |
| `/history` | show the conversation so far |
| `/system <text>` | set or replace the system prompt |
| `/think on\|off` | toggle the thinking block mid-session |
| `/tools` | list tools offered by the MCP server |
| `/prompt` | print the exact rendered prompt |
| `/stats` | messages and prompt tokens vs context |
| `/quit`, `/exit` | leave |

`/prompt` is the useful one when something looks wrong — it shows
precisely what the model is being fed, template and all.

---

## Real Jinja templates

Earlier versions hand-wrote the ChatML layout in C. That worked for
Qwen3.5 and would have broken on anything else. `infer` now includes a
Jinja2 subset interpreter (`src/jinja.c`, `src/jinja_eval.c`) and renders
`tokenizer.chat_template` straight from the GGUF.

Supported: `{{ }}` with `-` whitespace control, `if`/`elif`/`else`,
`for` with `loop.index0/first/last/previtem/nextitem`, `set` (inline and
block), `macro` with defaults and keyword arguments, `namespace()`,
filters (`trim` `tojson` `string` `length` `list` `first` `last`
`default` `upper` `lower` `safe`), tests (`is string/mapping/iterable/
defined/none/number/boolean`, with `not`), operators
(`and or not == != < > <= >= + - * / % ~ in`, ternary `if/else`), string
methods (`.split` `.startswith` `.endswith` `.strip` `.lstrip`
`.rstrip`), `seq[::-1]`, indexing, attribute access, and
`raise_exception()`.

**Verified byte-identical to Python's Jinja2** on the Qwen3.5 template,
for plain chat and for tool-enabled prompts, with thinking on and off.
`make test` runs `t_jinja`, which checks 25 expression cases plus the
real template from a model file.

Two details that cost real debugging and are worth knowing if you touch
this code:

* `{{-` trims *literal template text* only, never the output of an
  expression, and never past the start of the current loop iteration.
  Getting this wrong silently deletes the newline after every message.
* `|tojson` **sorts object keys**, because Jinja2's does. Without that
  the tools prompt differs byte-for-byte from every other runtime.

---

## MCP tools

```sh
infer chat model.gguf --mcp http://127.0.0.1:3000/mcp
```

On start-up `infer` runs the MCP handshake (`initialize`,
`notifications/initialized`, `tools/list`), passes the discovered tools
into the chat template's `tools` variable, and prints what it found:

```
connecting to MCP server http://127.0.0.1:3000/mcp ...
  test-mcp: 2 tools
    get_weather              Get the current weather for a city
    add                      Add two numbers
```

When the model emits a tool call, `infer` parses it, invokes the tool
over JSON-RPC, appends the result as a `tool` message, and lets the
model continue — up to 4 rounds per turn:

```
> What is the weather in Springfield?
  [tool] get_weather {"city":"Springfield"}
  [ -> ] Weather in Springfield: 14 C, light rain, wind 12 km/h.
It is 14 C and lightly raining in Springfield.
```

### Transport

**HTTP only** (Streamable HTTP), as requested. Both a plain JSON body
and an SSE (`data:` line) response are handled, as is `Mcp-Session-Id`.

**stdio transport is not implemented.** It needs child-process spawning
and pipe plumbing, which has no portable ANSI C form and would require
entirely separate POSIX and Win32 backends.

**https:// is not supported** — there is no TLS stack in this project,
and adding one would break the no-dependencies rule. Point `infer` at a
local HTTP endpoint, or put a proxy in front.

### Tool-call parsing

Qwen3.5 emits calls as:

```
<tool_call>
<function=NAME>
<parameter=KEY>
value
</parameter>
</function>
</tool_call>
```

The parser also accepts a bare `<function=...>` block without the
`<tool_call>` wrapper, because small models frequently omit it or get
truncated mid-call. Repeated parameters keep the last value, since a
model that corrects itself mid-call would otherwise produce a JSON
object with duplicate keys. Values that look like numbers, booleans,
`null`, objects or arrays are emitted as JSON literals; everything else
is quoted.

A caveat worth stating: a 0.8B model is not reliable at tool use. In
testing it called the right tool with roughly the right arguments, then
sometimes mis-stated the result. The plumbing is correct; the model is
small.

---

## Performance note

Every turn re-renders the whole conversation and re-ingests it, so the
model sees exactly what its template defines rather than an
approximation. The cost is that prompt processing grows with history —
on a Geode LX that dominates. `/forget` resets it, and `/stats` shows
how many tokens you are carrying.
