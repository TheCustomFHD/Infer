# `src/agent.c` / `src/agent.h` — prompts and tools, shared by every mode

**~550 lines total.** Turning messages into a prompt, spotting a tool call in
the model's output, and running it.

`chat`, `run` and `serve` all need these three things. They used to live
inside `chat.c`, which is the direct reason `--system`, `--think` and
`--mcp` worked only in chat mode. Moving them here made those options
general, and means a fix applies to all three modes at once.

---

## What it provides

| function | purpose |
|---|---|
| `agent_messages()` | a new empty message list |
| `agent_add_msg()` | append `{role, content}` |
| `agent_set_system()` | insert or replace the system turn at position 0 |
| `agent_has_system()` | does the list already begin with system/developer? |
| `agent_render()` | messages → prompt, template or raw |
| `agent_parse_tool_call()` | find the first tool call in generated text |
| `agent_run_tool()` | invoke it over MCP |
| `agent_connect_mcp()` | connect and report what is offered |
| `agent_js_to_jj()` | JSON value → Jinja value |

---

## Rendering

```c
typedef struct {
    const char *tmpl;
    int         raw;
    int         thinking;
    mcp_client *mcp;
    int         tool_rounds;
    int         show_tools;
} agent_config;
```

Every mode fills this in from `infer_opts` and gets identical behaviour.
The template path binds `messages`, `add_generation_prompt`,
`enable_thinking` and — when an MCP server is attached — `tools`.

A regression test asserts the important consequence: a conversation
built by `chat` and the same conversation arriving as OpenAI JSON at
`serve` render to **byte-identical prompts**.

### `--raw`

Skips the template entirely and joins message contents with blank lines.
No role prefixes are invented: `"User:"` markers would be a chat
template by another name, just an undocumented one.

This is what base models want, and what harnesses that refuse Jinja —
`llama-bench` among them — need. It is also the only mode that works
when a GGUF carries no chat template at all.

---

## Tool-call parsing

Qwen3.5 emits tool calls in **two** shapes, chosen by the template's
`tool_call_format`. Both are accepted:

**XML** (the default):

```
<tool_call>
<function=get_weather>
<parameter=city>
Springfield
</parameter>
</function>
</tool_call>
```

**JSON** (`tool_call_format='json'`):

```
<tool_call>
{"name": "get_weather", "arguments": {"city": "Springfield"}}
</tool_call>
```

The OpenAI nesting `{"function": {"name": …, "arguments": …}}` and
double-encoded arguments (a JSON string containing JSON) are handled
too.

> **A bug worth recording.** Only the XML form was parsed at first. When
> the model produced the JSON form, the parser still matched — it found
> no `<parameter=` tags and returned an *empty* argument object. The
> tool ran with no arguments and returned a plausible-looking wrong
> answer (`add {}` → "The sum is 0"), with nothing in the logs to
> suggest a parse failure. Silent argument loss is far worse than a
> refusal; both forms are now tested explicitly in `tests/t_agent.c`.

The parser also tolerates what small models actually do under
truncation: a missing `</tool_call>`, a bare `<function=…>` block with
no wrapper, and the same parameter emitted twice (the later value wins,
since JSON objects may not carry duplicate keys).

---

## Why `agent_js_to_jj` matters

`server.c` used to copy only `role` and `content` out of each incoming
message. Templates read `message.tool_calls` and
`message.reasoning_content`, so a tool-using conversation replayed
through the API silently lost its tool calls. Converting the whole JSON
object preserves every field.

---

## See also

- [opts-c.md](opts-c.md) — where the options come from
- [chat-c.md](chat-c.md) — the REPL that sits on top
- [server-c.md](server-c.md) — the HTTP layer and its tool loop
- [mcp-c.md](mcp-c.md) — the MCP client
- [jinja-c.md](jinja-c.md) — the template engine
