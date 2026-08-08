# `src/infer.h` — shared declarations

**~340 lines.** The one header every translation unit includes. Declares
the GGUF reader, the quantisation kernels, the tokeniser, the engine, the
sampler and the small utility layer.

## Portability decisions encoded here

```c
typedef double gg_off;
```

A 64-bit-capable byte offset. C89 has no `long long`, and on i486 `long`
is 32 bits — too small for GGUF offsets in a >2 GB file. `double` holds
integers exactly to 2^53, which is far beyond anything a 32-bit host can
map. Every file offset in the project is a `gg_off`.

```c
#define INFER_VERSION "1.8.0"
```

Overridable with `-DINFER_VERSION=...`.

## The main types

| type | purpose |
|---|---|
| `gg_tensor` | name, dims, quantisation type, resolved data pointer |
| `gg_kv` | one GGUF metadata entry (scalar, string or array) |
| `gguf_file` | the parsed container plus the mapped tensor blob |
| `strbuf` | growable string; used for every piece of assembled text |
| `sampler_params` | temperature, top-p, top-k, repeat penalty, seed |

## Opaque handles

`tokenizer`, `qwen35_model`, `qwen35_ctx` and `sampler` are forward
declarations only. Their layouts live in the corresponding `.c` files, so
nothing outside can depend on their internals.

## `strbuf`

Used everywhere text is assembled: HTTP responses, JSON bodies, rendered
templates, detokenised output.

```c
void sb_init(strbuf *b);
int  sb_add(strbuf *b, const char *s, size_t n);
int  sb_printf(strbuf *b, const char *fmt, ...);
int  sb_json_escape(strbuf *b, const char *s, size_t n);
```

**`sb_printf` uses `vsprintf` into a 2 KB stack buffer**, because C89 has
no `vsnprintf`. That is safe only because every call site formats short
fragments — headers, numbers, JSON scaffolding. User text always goes
through `sb_add` or `sb_json_escape`, never through `sb_printf`. If you
add a call, keep that rule.

## See also

- [gguf-c.md](gguf-c.md) · [util-c.md](util-c.md) ·
  [../ARCHITECTURE.md](../ARCHITECTURE.md)

## Byte order

`INFER_BIG_ENDIAN` is derived from the usual compiler predefines
(`__BYTE_ORDER__`, `__s390x__`, `__ARMEB__`, ...) and can be forced with
`-DINFER_BIG_ENDIAN=1` or `=0`.

It gates only the cold paths. The hot k-quant kernels read weights a
byte at a time on every host, so byte-order neutrality costs nothing
measurable — see [../FINDINGS.md](../FINDINGS.md) findings 18 and 19.
