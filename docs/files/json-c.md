# `src/json.c`, `src/json.h` — JSON parser

**~320 lines.** Read-only recursive-descent parser, enough of RFC 8259
to read OpenAI request bodies and MCP responses.

## API

```c
js_val *js_parse(const char *text);
void    js_free(js_val *v);
js_val     *js_get (const js_val *o, const char *key);
const char *js_str (const js_val *o, const char *key, const char *dflt);
double      js_num (const js_val *o, const char *key, double dflt);
int         js_bool(const js_val *o, const char *key, int dflt);
```

The accessors take defaults, so callers rarely need null checks:

```c
int stream = js_bool(req, "stream", 0);
double temp = js_num(req, "temperature", 0.7);
```

## Details

* `\uXXXX` escapes are decoded, including surrogate pairs, and re-encoded
  as UTF-8.
* Depth is capped at 64 to bound recursion on hostile input.
* Numbers go through `strtod`; `JS_BOOL` reuses the `num` field.
* Objects keep **insertion order** and permit duplicate keys — the last
  wins on lookup. (Contrast `|tojson` in the Jinja engine, which must
  *sort* keys to match Jinja2.)

## Not supported

Serialisation. Output JSON is assembled with `strbuf` and
`sb_json_escape` at each call site, which is smaller and avoids building
a tree only to flatten it. `mcp.c` has a private `js_dump()` for the one
case that needs to echo a parsed value back verbatim (tool schemas).

## See also

- [server-c.md](server-c.md) · [mcp-c.md](mcp-c.md)
