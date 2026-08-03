# `tools/` — development helpers

**None of these are needed to build or run `infer`.**

## `gen_unicode.py`

Regenerates `src/unicode_tbl.h`, the codepoint ranges for Unicode
general categories L (letter), M (mark) and N (number), which the
tokeniser's pre-split regex needs.

```sh
python3 tools/gen_unicode.py
```

The generated header is **checked in**, so building never requires
Python. Only re-run this when targeting a new Unicode version.

Output: 660 + 182 + 143 ranges, binary-searched at run time.

## `ref_numpy.py`

An independent NumPy implementation of the whole `qwen35` forward pass,
written from the reference graph rather than from this C code. Used to
validate the engine.

```sh
python3 tools/ref_numpy.py model.gguf "760,6511,314,9338,369"
```

Prints the top-10 logits. Both implementations agree exactly.

It dequantises weights on demand and computes the LM head in row chunks,
so it runs in modest RAM despite the 248,320 × 1024 embedding table.

## `mcp_test_server.py`

A minimal MCP server over Streamable HTTP with two toy tools
(`get_weather`, `add`), for exercising `infer chat --mcp`.

```sh
python3 tools/mcp_test_server.py 3000
infer chat model.gguf --mcp http://127.0.0.1:3000/mcp
```

Implements `initialize`, `notifications/initialized`, `tools/list` and
`tools/call` — enough to verify the client's handshake, discovery and
call loop.

## See also

- [tokenizer-c.md](tokenizer-c.md) · [mcp-c.md](mcp-c.md)
