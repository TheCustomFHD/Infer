# `src/util.c` — allocation, strings, logging, file loading

**~270 lines.** Small shared helpers.

## Allocation

`xmalloc`, `xcalloc`, `xrealloc`, `xstrdup` abort with a clear message on
failure rather than returning NULL. On a machine where a failed
allocation means the model does not fit, there is nothing useful to do
with the error, and every call site is simpler for it.

## `strbuf`

Growable string, doubling on demand. Used for every assembled text:
HTTP responses, JSON, rendered templates, detokenised output.

`sb_json_escape()` handles the JSON string escapes and passes UTF-8
bytes ≥ 0x20 through unchanged, which is legal and keeps output
readable.

**`sb_printf` uses `vsprintf` into a 2 KB stack buffer.** C89 has no
`vsnprintf`. This is safe only because every call site formats short
fragments — headers, numbers, JSON scaffolding. **User text must go
through `sb_add` or `sb_json_escape`.** Keep that rule if you add calls.

## `load_text_file`

Reads a whole text file for `--template`. Caps at 8 MB, and normalises
CRLF to LF — templates are frequently stored with Windows line endings
and the literal text ends up in the prompt.

## Little-endian scalar readers

`inf_rd_f32p`, `inf_rd_f64p` and `inf_rd_f32v` read GGUF's
little-endian floats regardless of host byte order. On a little-endian
host each is a plain `memcpy` that the compiler folds into a single
load; on big-endian the bytes are reversed explicitly.

`memcpy` rather than a pointer cast on purpose: the blob is only
byte-aligned, so a cast would risk an alignment fault (SPARC, older ARM)
as well as violating strict aliasing.

## See also

- [infer-h.md](infer-h.md)
