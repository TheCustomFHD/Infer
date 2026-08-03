# `src/gguf.c` — GGUF container reader

**~470 lines.** Parses the header, the key/value metadata block and the
tensor directory, then makes the tensor data available.

## Format

```
"GGUF"  u32 version  u64 n_tensors  u64 n_kv
n_kv   x  { string key, u32 type, value }
n_tens x  { string name, u32 n_dims, u64 dims[], u32 type, u64 offset }
padding to general.alignment (default 32)
tensor data blob
```

All little-endian, by definition of the format. The reader assembles
every multi-byte value from bytes, so a big-endian host reads the same
file correctly — see "Byte order" below.

## Reading 64-bit values without 64-bit integers

```c
static int rd_u64(FILE *f, double *v) {
    unsigned long lo, hi;
    rd_u32(f, &lo); rd_u32(f, &hi);
    *v = (double) lo + (double) hi * 4294967296.0;
}
```

Exact for anything under 2^53. `rd_i64` handles the negative case by
two's-complement magnitude.

## Numeric arrays are stored as `float`, not `double`

`tokenizer.ggml.token_type` has 248,320 entries. As `double` that is
1.9 MB; as `float`, 0.95 MB. On a 32-bit host with a 500 MB model already
mapped, that halving is worth having, and the values are small integers
so no precision is lost.

## Memory mapping

```c
g->map = sys_map_file(path, g->data_start, blob_end);
if (g->map) { g->blob = sys_map_data(g->map); g->mapped = 1; }
else        { /* fall back to malloc + fread */ }
```

Mapping is what makes a 497 MB model usable on a machine with less RAM
than that: pages are demand-loaded and, being clean, can be evicted
without ever touching swap. Startup is immediate — there is no load-time
copy.

The `malloc` fallback exists for platforms where mapping fails or is
unavailable (see the Windows 95 note in
[../FINDINGS.md](../FINDINGS.md)).

## Safety

Every count is sanity-checked before allocation: 100,000 tensors/keys
max, 20M array elements, 64 MB per string, and a hard refusal above
1900 MB of tensor data since that cannot fit a 32-bit address space.
A truncated or hostile file produces a clear message rather than a
crash.

## Byte order

GGUF stores every multi-byte value little-endian. `rd_u16/u32/u64/i64`
assemble them from bytes with explicit shifts, and `rd_f32`/`rd_f64` go
through `inf_rd_f32p`/`inf_rd_f64p`, so the reader is correct on either
host and free on a little-endian one.

`gguf_f32data(t)` returns an F32 tensor in host order. On little-endian
that is `(const float *) t->data` — the mapping used directly, nothing
copied. On big-endian the blob is mapped read-only, so one swapped copy
per tensor is made at first use and cached in `gg_tensor.swapped`
(1.9 MB for a 0.8B k-quant model; freed by `gguf_close`).

## See also

- [sys-h.md](sys-h.md) for the mapping abstraction
- [quant-c.md](quant-c.md) for what the blob contains
- [util-c.md](util-c.md) for the byte-wise readers
