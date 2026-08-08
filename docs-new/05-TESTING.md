# Testing

Twenty test programs in four groups. **Running only `make test`
leaves most of the kernel tests unbuilt** — that is how a stale
assertion and a threading race both shipped.

- [The four groups](#the-four-groups)
- [What each test proves](#what-each-test-proves)
- [Running everything](#running-everything)
- [Running the CI suite locally](#running-the-ci-suite-locally)
- [What CI asserts beyond the tests](#what-ci-asserts-beyond-the-tests)
- [Coverage gaps](#coverage-gaps)

---

## The four groups

```sh
make test             # 9 programs, several need a model
make i8-split-test    # 8 host-side kernel tests, no model needed
make vis-shim-test    # VIS kernels vs i8 on any host, through a shim
make solaris-test     # VIS assumptions, Sun Studio, on the real machine
```

**After any kernel change, run `make i8-split-test`.** It is seconds,
needs no model, and covers the things most likely to break.

## What each test proves

### `make test` — the nine

| test | model? | proves |
|---|---|---|
| `t_align` | no | MMX constants are 8-byte aligned. Misalignment is invisible on x86 and faults on a Geode |
| `t_jinja` | optional | 25 expression cases plus the real chat template |
| `t_quant` | yes | dequantised values match a reference decoder |
| `t_tokenizer` | yes | 14 round-trips: CJK, emoji, contractions, whitespace |
| `t_engine` | yes | end-to-end greedy generation |
| `t_backend` | yes | every backend vs `ref`, accuracy **and** throughput |
| `t_agent` | no | chat/run/serve render byte-identical prompts; every tool-call shape parses **with its arguments** |
| `t_endian` | optional | byte-order neutrality; a big-endian build prints the same numbers |
| `t_cache` | yes | prompt-prefix reuse gives bit-identical logits to a fresh decode |

### `make i8-split-test` — the eight host-side kernel tests

None need a model; all run in seconds.

| test | proves |
|---|---|
| `t_ident` | `i8` and `mmx` agree **bit-for-bit** on every format. Deliberately excludes `avx2` |
| `t_avx2acc` | `avx2` is at least as close to float `ref` as `i8` is |
| `t_avx2sat` | `VPMADDUBSW`'s int16 lanes cannot saturate. Q6_K is the tight one at 16002/32767 (48.8%) |
| `t_scales` | the word-parallel 6-bit scale decode matches the branchy reference exactly |
| `t_i8split` | the `i8` Q4_K split-loop reassociation is integer-exact |
| `t_q5split` | ditto Q5_K, whose loop shape differs between scalar and vectorised builds |
| `t_thread` | threading changes the schedule, never the arithmetic — **every backend by name**, at 1/4/8 threads |
| `t_kbench` | isolated per-format kernel throughput (timing, not pass/fail) |

### `make vis-shim-test` and `make solaris-test`

| test | proves |
|---|---|
| `t_vis` | the `fmul8x16` exactness identity the VIS kernels rest on |
| `t_viskern` | VIS kernels vs `i8` on synthetic blocks. Should read `0.000e+00` |
| `t_vissat` | worst-case lane saturation: every weight max, every activation at the ±127 clamp |

`tests/vis_shim/vis_proto.h` reimplements Sun's prototypes in plain C,
so `-D__SUNPRO_C -Itests/vis_shim` takes the Studio branch under GCC.
That catches the type errors which are most of the risk in maintaining
two compiler paths. **It proves types and arithmetic only** — GCC
inlines the shim's C instead of emitting VIS, so instruction counts
taken this way are meaningless.

## Two subtleties that matter

### Why `t_ident` skips `avx2`

Since 1.20.0 the AVX2 k-quant kernels quantise the activation per 256
values instead of per 32, reassociate the sum across lanes, and apply
the scale before converting to float. That is the same mathematics in
a different order, and it is **not** bit-identical. Measured against
`i8` on synthetic worst-case weights:

```
Q4_K   NOT bit-identical   (8/8 rows differ, max abs delta 5)
Q5_K   NOT bit-identical   (8/8 rows differ, max abs delta 15.7)
Q6_K   NOT bit-identical   (8/8 rows differ, max abs delta 18.4)
Q8_0   bit-identical       (uses the same per-32 plane as i8)
```

Bounding that error is `t_avx2acc`'s job. **Do not "fix" `t_ident` by
switching the avx2 comparison on** — it will report failures that are
not defects.

### The reference matters in `t_viskern`

It compares against `qmv_i8`, deliberately. Against `qmv_ref` the VIS
kernels show 1–12% relative error — that is int8 activation
quantisation, visible in `i8` too, not a kernel fault. Using the wrong
reference sends you debugging correct code.

## Running everything

```sh
# host-side, fast, no model
make i8-split-test
for t in t_ident t_avx2acc t_avx2sat t_scales t_thread t_i8split t_q5split; do
  printf '%-12s ' $t; ./build/$t >/dev/null 2>&1 && echo PASS || echo FAIL
done
./build/t_thread 1; ./build/t_thread 4; ./build/t_thread 8

# with a model
make test
M=model.gguf
for t in t_quant t_tokenizer t_engine t_cache; do ./build/$t $M; done
for t in t_align t_agent t_jinja t_endian; do ./build/$t; done

# cross-platform under qemu
qemu-sparc64-static -L /usr/sparc64-linux-gnu ./t_thread_sparc 4
qemu-s390x-static -L /usr/s390x-linux-gnu ./t_endian_be

# Windows, under Wine — see 02-BUILDING.md for the prefix rules
export WINEPREFIX="$HOME/.cache/wineprefix" WINEDEBUG=-all
for T in 1 2 4 8; do
  wine ./infer-<v>-windows64.exe run 'Z:\path\model.gguf' -n 12 -t 0 -p hi -T $T
done
pkill -9 wineserver
```

## Running the CI suite locally

CI is one workflow, `.github/workflows/build.yml`, with ~51 steps.
**Extract and run them all** — spot-checking is what let a stale
assertion and a threading race ship:

```sh
python3 - <<'PY' > /tmp/ci.sh
import yaml
y = yaml.safe_load(open('.github/workflows/build.yml'))
print("#!/bin/bash\nset -u")
print('export LINUX=infer-1.23.2-linux LINUX64=infer-1.23.2-linux64')
print('export WIN=infer-1.23.2-windows.exe WIN64=infer-1.23.2-windows64.exe')
print('fails=0')
for jn, j in y['jobs'].items():
    for st in j.get('steps', []):
        run = st.get('run')
        if not run or 'apt-get' in run:
            continue
        print('echo "== %s =="' % st.get('name', '?'))
        print("cat >/tmp/step.sh <<'CIEOF'"); print(run); print("CIEOF")
        print('bash /tmp/step.sh >/tmp/s.log 2>&1 && echo "  PASS" '
              '|| { echo "  FAIL"; tail -8 /tmp/s.log; fails=$((fails+1)); }')
print('echo "FAILURES: $fails"')
PY
bash /tmp/ci.sh
```

**Three steps always fail locally and that is expected**: `resolve
version and binary names`, `tag matches VERSION` and `assemble the
release` need `$GITHUB_OUTPUT`/`$GITHUB_ENV` or a tag ref. Everything
else must pass.

## What CI asserts beyond the tests

These are guards on properties no unit test can see:

| guard | why it exists |
|---|---|
| one binary, 486 baseline outside the gated AVX2 object | >100 ymm **and** >100 MMX **and** xgetbv in the shipped exe |
| baseline objects contain no CMOV and no SSE/AVX | each object compiled alone and disassembled |
| the AVX2 kernel really uses VPMADDUBSW | the whole point of that backend |
| no static scratch in the backends | a `static` accumulator raced under threading and produced fluent gibberish |
| threading defaults to one thread | opt-in is a safety property |
| the spin budget is conditional on the core count | unconditional spinning cost 40% when oversubscribed |
| both tpool waits re-check under a loop | a bare `if` on the Win32 path crashed Windows at `-T 2` |
| `NO_THREADS=1` compiles the pool away | 0 `pthread_create` references |
| AVX2 build does not contract floats | `-ffp-contract=off`; fusion diverges output at ~token 8 |
| windows exe imports only XP-safe DLLs | three DLLs, PE32 subsystem 4.00 |
| strict C90, no `long long` | naming a 64-bit type broke Solaris twice |
| docs match the code | flags, targets, findings count and numbering |

Every guard was verified with a **negative control** — deliberately
break the property and confirm the guard fires. A guard that has never
failed is a guard you have not tested.

## Coverage gaps

Stated plainly:

- **Real Windows** is still untested. Wine is close but not identical,
  especially for synchronisation costs.
- **Real Solaris/SPARC hardware** is untested here; only qemu and the
  Studio shim.
- `t_vis`/`t_viskern` fail under the CI runner's qemu 8.2.2 and pass
  on 10.0.11. Non-blocking, prints mismatches, cannot be reproduced
  locally on Debian trixie.
- No fuzzing of the GGUF parser or the HTTP surface.
- No test asserts end-to-end output quality — only determinism and
  cross-backend agreement.
