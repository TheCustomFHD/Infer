# `tests/` — the test suite

There are four groups, built by four different targets. Running
only `make test` leaves most of the kernel tests unbuilt, which is how
a stale assertion and a threading race once shipped.

```sh
make test             # 9 programs, several need a model
make i8-split-test    # 8 host-side kernel tests, no model needed
make vis-shim-test    # VIS kernels vs i8 on any host
make solaris-test     # VIS assumptions, Sun Studio, on the real machine
```

### `make test` — the nine

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
| `t_cache` | yes | prompt-prefix reuse gives bit-identical logits to a fresh decode |

### `make i8-split-test` — the eight host-side kernel tests

None of these need a model, and all of them run in seconds. This is
the group to run after touching any kernel.

| test | what it proves |
|---|---|
| `t_ident` | `i8` and `mmx` agree **bit-for-bit** on every format. Deliberately excludes `avx2` — see below |
| `t_avx2acc` | `avx2` is at least as close to the float `ref` as `i8` is. This is the accuracy claim that replaces bit-identity for the AVX2 k-quants |
| `t_avx2sat` | `VPMADDUBSW`'s int16 lanes cannot saturate on any format fed to them. Q6_K is the tight one at 16002/32767 |
| `t_scales` | the word-parallel 6-bit scale decode matches the branchy reference exactly (finding 53) |
| `t_i8split` | the `i8` Q4_K split-loop reassociation is integer-exact |
| `t_q5split` | ditto for Q5_K, whose loop shape differs between scalar and vectorised builds |
| `t_thread` | threading changes the schedule, never the arithmetic — every backend, by name, at 1/4/8 threads |
| `t_kbench` | isolated per-format kernel throughput (timing, not pass/fail) |

**Why `t_ident` skips `avx2`.** Since 1.20.0 the AVX2 k-quant kernels
quantise the activation per 256 values instead of per 32, reassociate
the sum across lanes, and apply the scale before converting to float.
That is the same mathematics in a different order, and it is *not*
bit-identical: measured against `i8` on synthetic worst-case weights,
`Q8_0` matches exactly while `Q4_K`/`Q5_K`/`Q6_K` differ. Bounding
that error is `t_avx2acc`'s job. Do not "fix" `t_ident` by switching
the comparison on — it will report failures that are not defects.

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

## `t_viskern` — VIS kernels against `i8`, without a model

`t_vis` proves the *identity* the kernels rest on. `t_viskern` proves
the **kernels** themselves, on synthetic Q4_K and Q5_K blocks at three
sizes each, so it runs in seconds on a 440 MHz Ultra 10 and needs no
download:

```
Q4_K     ncols=3584 rows=4    worst rel err 0.000e+00  ok
Q5_K     ncols=3584 rows=4    worst rel err 0.000e+00  ok
```

Zero, not "small". The exactness trick means VIS and `i8` perform the
same integer arithmetic, so any nonzero result is a bug.

The reference is `qmv_i8`, deliberately. Against `qmv_ref` the VIS
kernels show 1–12% relative error — that is int8 activation
quantisation, visible in `i8` too, not a kernel fault. Using the wrong
reference sends you debugging correct code.

### Testing the Sun Studio path without a Sun Studio

`tests/vis_shim/vis_proto.h` reimplements Sun's prototypes in plain C.
Compiling with `-D__SUNPRO_C -Itests/vis_shim` takes the Studio branch
of `backend_vis.c` under GCC, which catches the type errors that make
up most of the risk in maintaining two compiler paths:

```sh
gcc -std=gnu89 -O2 -m64 -mcpu=ultrasparc \
    -D__SUNPRO_C=0x5150 -Itests/vis_shim \
    -DINFER_HAVE_VIS -DINFER_BIG_ENDIAN=1 -Isrc \
    -o /tmp/t_viskern_sun tests/t_viskern.c src/backend_vis.c \
    src/backend.c src/quant.c src/util.c src/prof.c \
    src/gguf.c src/sys_posix.c -lm
```

It proves types and arithmetic only. GCC inlines the shim's C instead
of emitting VIS instructions, so instruction counts taken this way are
meaningless — Studio's code generation can only be judged by Studio.

## `t_vissat` — worst-case lane saturation

The kernels accumulate in 16-bit lanes and widen only when the budget
runs out. Q6_K sits at 32004 of 32767 — a 2.3% margin — so the bound is
tested, not asserted. Every weight at maximum, every activation at the
±127 clamp, bit-identity with `i8` required:

```
Q6_K all-max saturation: bit-identical
Q5_K all-max saturation: bit-identical
```

Needs no model.

## Beyond the suite

Two checks were run during development and are worth repeating after
kernel changes:

* **Exhaustive backend sweep** — all 320 tensors × 6 activation patterns
  (uniform, large, tiny, sparse, alternating, all-zero) × both fast
  backends against `ref`. 2,244 comparisons, zero failures. This was a
  development-time sweep with `t_backend`, not something `make test`
  reruns.
* **Jinja byte-comparison** — render a template with both this engine and
  Python's Jinja2 and `cmp` the output. Done for the built-in Qwen3.5
  template and the 16 kB froggeric community template, with and without
  tools, thinking on and off.

## See also

- [../COMPILE.md](../../COMPILE.md) · [../FINDINGS.md](../FINDINGS.md)
