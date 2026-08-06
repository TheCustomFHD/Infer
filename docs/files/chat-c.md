# `src/chat.c` — interactive multi-turn chat

**~480 lines total.** The `infer chat` REPL: conversation state, the command
set, streaming, and the turn loop.

Prompt rendering and tool calling moved to [`agent.c`](agent-c.md), which
`serve` and `run` share — which is why every option this mode accepts now
works in those too. What remains here is the interactive part.

## Turn structure

1. Read a line. If it starts with `/`, handle it as a command.
2. Append `{role:"user", content:...}` to the message list.
3. Render the **whole conversation** through the chat template.
4. Tokenise, reset the engine, ingest, stream the reply.
5. If the reply contains a tool call and MCP is connected: execute it,
   append the result as a `tool` message, and loop (`--tool-rounds`,
   default 4).
6. Otherwise append the reply as `assistant` and wait for input.

Re-rendering and re-ingesting every turn means the model sees exactly
what its template defines — no incremental approximation that drifts.
The cost is that prompt processing grows with history, which on a Geode
dominates. `/stats` shows the token count; `/forget` resets it.

## Commands

`/help` `/new` `/clear` `/forget` `/history` `/system` `/think`
`/raw` `/tools` `/prompt` `/stats` `/perf` `/set` `/quit` `/exit`

`/prompt` prints the exact rendered prompt — the first thing to reach for
when output looks wrong.

`/set` lists the options in effect, under their command-line names.
`/think on|off` and `/raw on|off` change mid-conversation what `--think`
and `--raw` set at startup.

`-p/--prompt` answers one turn before the REPL opens, so a scripted
opener and an interactive session are the same command.

With `--log-perf`, each turn is followed by its TTFT, tokens/second and
seconds/token, and leaving prints a **session total**; `/perf` shows the
running total without waiting. On a slow machine the per-turn prompt
figure visibly grows with history, which is the cost `/forget` resets.

## Tool-call parsing

Now `agent_parse_tool_call()`, shared with `serve` and `run` — see
[agent-c.md](agent-c.md) for the two wire formats and the tolerances.

## Honest limitation

A 0.8B model is not reliable at tool use. In testing it called the right
tool with roughly the right arguments and then sometimes mis-stated the
result. The plumbing is correct; the model is small.

## Streaming and UTF-8

Same problem as the server: BPE splits multi-byte characters. The chat
loop buffers bytes and prints only complete UTF-8 sequences.

## See also

- [agent-c.md](agent-c.md) · [opts-c.md](opts-c.md) ·
  [mcp-c.md](mcp-c.md) · [jinja-c.md](jinja-c.md) ·
  [../../USER-GUIDE.md](../../USER-GUIDE.md)
