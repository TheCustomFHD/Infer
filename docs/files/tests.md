# `tests/` — the test suite

`make test` builds all eight into `build/`.

| test | needs a model? | what it proves |
|---|---|---|
| `t_align` | no | MMX constants are 8-byte aligned |
| `t_jinja` | optional | 25 expression cases + the real chat template |
| `t_quant` | yes | dequantised values match a reference decoder |
| `t_tokenizer` | yes | 14 round-trips: CJK, emoji, contractions, whitespace |
| `t_engine` | yes | end-to-end greedy generation |
| `t_backend` | yes | every backend vs `ref`, accuracy **and** throughput |
| `t_agent` | no | chat/run/serve render identical prompts; every tool-call shape parses |
| `t_endian` | optional | byte-order neutrality; same numbers on a big-endian host |

## `t_agent` — the one that guards cross-mode consistency

Ten assertions. The two that matter most:

* A conversation built the way `chat` builds it, and the same
  conversation arriving as OpenAI JSON the way `serve` receives it,
  render to **byte-identical prompts**. That is the property which makes
  "the same flag means the same thing in every mode" true rather than
  merely intended.
* Every tool-call shape the model actually emits parses **with its
  arguments** — XML, JSON, the OpenAI `{"function":{…}}` nesting, and
  truncated variants. This test exists because the JSON form was once
  parsed to an empty argument object, which is a silent wrong answer
  rather than a visible failure.

## `t_endian` — the one that needed a different machine

Checks that known little-endian bit patterns decode to the right floats,
and (with a model) prints real tensor values that must match a
little-endian run byte for byte.

The interesting part is what it was written *after*. A source audit
predicted three broken sites and cleared `q_fp16_to_fp32()` as
endianness-neutral; the cleared function was the actual bug, and the
three predicted ones were merely also broken. See finding 18.

Runs anywhere. To exercise the case it exists for:

```sh
s390x-linux-gnu-gcc -std=c89 -O2 -static -o t_endian_be \
  tests/t_endian.c src/gguf.c src/quant.c src/backend.c \
  src/util.c src/prof.c src/sys_posix.c -lm
qemu-s390x-static ./t_endian_be model.gguf
```

## `t_align` — the one that guards a real bug

MMX constants must be 8-byte aligned for `movq`. Misalignment is
**invisible on an out-of-order x86** (it splits the load silently) but
fails on real Geode/XP hardware. This asserts it at run time so a
compiler that ignores the alignment attribute is caught by `make test`
rather than by a user's crash.

## `t_backend` — accuracy and speed together

```
blk.0.ffn_gate.weight    Q4_K  512x1024
   ref     1462 Mflop/s   rel-l2 0.000e+00
   i8      2085 Mflop/s   rel-l2 3.624e-03
   mmx     4279 Mflop/s   rel-l2 3.624e-03
```

`rel-l2 ≈ 3.6e-3` is expected: that is the cost of int8 activation
quantisation, the same precision GGML uses, far below the error already
introduced by 4-bit weights. `ref` is 0 by definition.

## Beyond the suite

Two checks were run during development and are worth repeating after
kernel changes:

* **Exhaustive backend sweep** — all 320 tensors × 6 activation patterns
  (uniform, large, tiny, sparse, alternating, all-zero) × both fast
  backends against `ref`. 2,244 comparisons, zero failures.
* **Jinja byte-comparison** — render a template with both this engine and
  Python's Jinja2 and `cmp` the output. Done for the built-in Qwen3.5
  template and the 16 kB froggeric community template, with and without
  tools, thinking on and off.

## See also

- [../COMPILE.md](../../COMPILE.md) · [../FINDINGS.md](../FINDINGS.md)
