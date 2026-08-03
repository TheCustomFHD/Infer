# `src/mcp.c`, `src/mcp.h` — MCP client

**~505 lines.** Model Context Protocol client over **Streamable HTTP**.
All networking through [net-h.md](net-h.md), so it works on XP.

## Protocol

JSON-RPC 2.0 over HTTP POST:

```
POST /path HTTP/1.1
Content-Type: application/json
Accept: application/json, text/event-stream
MCP-Protocol-Version: 2025-06-18
Mcp-Session-Id: <if the server issued one>

{"jsonrpc":"2.0","id":N,"method":"...","params":{...}}
```

Handshake: `initialize` → `notifications/initialized` → `tools/list`.
Invocation: `tools/call`.

## Response handling

`extract_json()` copes with three shapes:

* a plain JSON body after the headers
* an **SSE** stream, where the payload is on `data:` lines — the last one
  is used
* `Transfer-Encoding: chunked`, where the size line is skipped

## Tool metadata

`inputSchema` is captured **verbatim** by re-serialising the parsed JSON
(`js_dump`), so it reaches the template exactly as the server wrote it.
`mcp_tools_as_jinja()` converts the list into the OpenAI-shaped
`{"type":"function","function":{...}}` dicts that chat templates expect.

## Deliberate limitations

**No stdio transport.** It requires spawning child processes and wiring
pipes, which has no portable ANSI C form and would need entirely
separate POSIX and Win32 backends. Explicitly out of scope.

**No https.** There is no TLS stack in this project and adding one would
break the no-dependencies rule. `mcp_connect()` detects an `https://`
URL and returns a clear message rather than failing obscurely.

## Testing

`tools/mcp_test_server.py` is a small Python MCP server with two toy
tools, used to verify the handshake, discovery and the full call loop.

## See also

- [chat-c.md](chat-c.md) · [net-h.md](net-h.md)
