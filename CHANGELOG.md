# Changelog

## 1.12.1 — Solaris build fix

### `sys_posix.c` would not compile with Sun Studio

Reported from a real UltraSPARC:

```
"src/sys_posix.c", line 50: invalid cast expression
cc: acomp failed for src/sys_posix.c
```

Line 50 was `off = (off_t) aligned;`. With `_FILE_OFFSET_BITS=64` on a
32-bit host, `off_t` is a **64-bit type** — `long long` on Solaris. C90
has no `long long`, so under `-Xc -xc99=none` a cast to it is a syntax
error, not a warning. GCC accepts it silently as an extension, which is
why every Linux build passed.

Fixed by never naming a 64-bit type in our source. The page-aligned
offset is kept as a `long` page *index* and multiplied by the page size
inside the `mmap()` call, so the compiler widens implicitly using
whatever `off_t` happens to be:

```c
page_index = (long) (offset / (double) pagesz);
...
base = mmap(NULL, maplen, PROT_READ, MAP_PRIVATE, fd, page_index * pagesz);
```

Verified with `gcc -std=c89 -pedantic -Werror=long-long -m32
-D_FILE_OFFSET_BITS=64`: clean. The only remaining occurrence of the
phrase "long long" in the codebase is the comment explaining this.

### Solaris flags updated from field use

`-xO5` and `-xunroll=16` adopted from a user's own flag set: the 4-issue
in-order UltraSPARC pipeline benefits from unrolling and neither flag
changes results.

Two flags from that set are deliberately **not** default:

* **`-fast`** is a macro that enables `-fsimple=2` and `-fns` — non-IEEE
  float with reassociation and flush-to-zero. The k-quant scales are
  fp16 values near zero and the DeltaNet recurrence accumulates over 128
  steps, so this is a real correctness risk rather than a theoretical
  one. Available as `make solaris-fast` for anyone who wants to try it,
  with `./build/t_backend model.gguf` to check the error terms.
* **`-xvis`** enables VIS intrinsics. There is no VIS code in the
  project, so it has no effect. Harmless, just pointless.

### CMOV: measured, not assumed

1.12.0 moved the MMX builds to a 486 baseline, which removes CMOV.
Measured cost of that on x86-64, MMX backend, identical work:

| build | best | median |
|---|---|---|
| `-march=i486` (0 CMOV) | 7.68 s | 7.70 s |
| `-march=geode` (30 CMOV) | 7.72 s | 7.72 s |

**0.6%, and in favour of the no-CMOV build** — i.e. noise. Of the 30
CMOVs the geode build emitted, only 3 were in a hot function
(`bk_quantize_x`, once per matvec); the rest were in template parsing,
server startup and `malloc` wrappers. There is nothing to gate: the 486
baseline is free.

---

## 1.12.0 — Solaris/SPARC support, and refusing to guess byte order

1.11.0 made the code byte-order neutral and verified it on s390x and
SPARC64 — both with GCC. A user then built it on a real UltraSPARC with
**Sun Studio 12.3** and got `!!!!!!!!!!` after eight minutes of
computation.

The code was correct. The *detection* was not.

### The bug

| | GCC / Clang | Sun Studio 12.3 |
|---|---|---|
| SPARC macro | `__sparc__` | `__sparc`, `__sparcv9` |
| byte order | `__BYTE_ORDER__` | not defined |
| unprefixed `sparc` | defined | **suppressed by `-Xc`** |

The `#if` tested `__sparc__` and `__BYTE_ORDER__`. Under `-Xc` on Sun
Studio neither exists, every condition was false, and the `#else`
selected little-endian — on a big-endian machine. Clean build, model
loaded, correct metadata, full speed, garbage output.

`1.0f` read byte-reversed is `4.6e-41`, so every quantisation scale in
the model became a denormal near zero.

### Fixed in two parts

**Detect properly.** Prefer `__BYTE_ORDER__`, then test every vendor
spelling: `__sparc`, `__sparcv8`, `__sparcv9`, `_BIG_ENDIAN`, `__hppa`,
`mc68000`, `_M_PPC`, plus an explicit little-endian list.

**Refuse to guess.** The `#else` is now `#error` with the remedy in the
message, so an unrecognised compiler fails to build rather than
silently choosing. And `gguf_open()` calls the new
`inf_check_byte_order()`, which compares `INFER_BIG_ENDIAN` against a
runtime probe and decodes a known bit pattern:

```
byte order mismatch: this binary was compiled for a little-endian host
but is running on a big-endian one.
  rebuild with -DINFER_BIG_ENDIAN=1
```

One second at startup, instead of eight minutes ending in noise.

### Solaris / SPARC build targets

```
make solaris              Sun Studio, auto-tuned to the build host
make solaris-gcc          GCC on Solaris
make solaris-profile      Sun Studio + stage timers
make solaris-gcc-profile  GCC + stage timers
make solaris-test         the test programs
```

These encode the two things that are easy to get wrong by hand:

* **`-lsocket -lnsl`** — Solaris keeps the socket API outside libc.
  Its absence is the most likely reason a GCC build failed to link
  where Sun Studio succeeded.
* **Sun Studio option spellings** — `-Xc -xc99=none` for strict C90,
  `-xO3`, `-xtarget=`. GCC's `-std=c89 -pedantic -O2` are not
  recognised by `cc`.

Cross-build for another machine with `make solaris SUNTARGET=ultra2`.

The portable `i8` backend is selected automatically on SPARC and is
**6.5–8.8× faster than `ref`** (measured per weight format under
emulation). There is no VIS backend yet.

### Versioned executables

Every binary now carries its version, so several builds can share a
directory without being renamed by hand:

```
infer-1.12.0            infer-1.12.0.exe
infer-1.12.0-i486       infer-1.12.0-mmx.exe
infer-1.12.0-geode      infer-1.12.0-profile.exe
infer-1.12.0-profile    infer-1.12.0-i486-profile
infer-1.12.0-geode-profile
```

`make version` prints it, `make checkversion` asserts it matches
`src/infer.h`, and `make SUFFIX=` restores the plain unversioned name.

### Makefile is POSIX again

The version was briefly extracted with `$(shell ...)`, which is a GNU
make extension that Solaris `/usr/ccs/bin/make` does not implement — it
would have produced binaries named `infer-`. Now a literal, guarded by
`make checkversion`. `:=` was likewise replaced with `=` throughout.

### MMX builds now have a 486 baseline

`make geode` and `make windows-mmx` were compiled `-march=geode`, which
let GCC emit **CMOV throughout ordinary code** — `xmalloc`, `gguf_open`,
`main`. CMOV is a Pentium Pro instruction; those binaries would fault on
a 486 or original Pentium despite the MMX kernels being runtime-gated.

Both now use `-march=i486 -mtune=geode -mmmx`: baseline 486, Geode
*scheduling*, MMX confined to `backend_mmx.c` behind its CPUID check.
Verified 0 CMOV instructions in our code, 354 MMX instructions still
present, both backends still bit-identical under greedy decoding.

**One binary now runs on a 486 and accelerates on a Geode**, which makes
the separate non-MMX build redundant for most users.

Honest caveat: mingw's C runtime still uses CMOV in `strtod`, `pow` and
`ldexp`, all of which we call. The Windows builds are therefore
Pentium-Pro-and-later in practice even though our own code is
486-clean. Documented in COMPILE.md rather than papered over.

### No bundled chat template

`templates/qwen-fixed-v21.3.jinja` has been removed. Third-party
templates are their authors' work to distribute; `templates/README.md`
now points at the source with a `curl` command instead.

### Documentation

`COMPILE.md` and `docs/ARCHITECTURE.md` rewritten in full. Every build
step is now a literal copy-pasteable command with its expected output,
including the verification commands for each target (486-purity, CMOV
count, DLL count, MMX constant alignment). ARCHITECTURE.md gains a
request walkthrough, the qwen35 hybrid layout, the k-quant block format,
and why the integer kernels are shaped the way they are.

### Also

* `t_endian` asserts `inf_check_byte_order()`
* Finding 20 in `docs/FINDINGS.md`

No engine, kernel or tokeniser changes. `t_engine` output is unchanged
and x86 performance is unaffected.

---

## 1.11.0 — runs on big-endian hosts

`infer` now produces bit-identical results on little- and big-endian
machines. Verified on real big-endian hardware emulation (s390x under
qemu), not by inspection.

Before: the s390x build loaded the model, printed correct metadata, and
generated `!!!!!!!!`.
After: `The capital of France is **Paris**.` — the same tokens x86 gives.

### The bug was not where the audit said it was

A source audit found three sites that read little-endian file bytes
through a host-order `memcpy` or pointer cast, and predicted those were
the whole problem. They were real and are fixed:

* `gguf.c` `rd_f32` / `rd_f64` — metadata scalars
* `quant.c` `dot_f32` and the F32 branch of `q_dequant_row`
* `qwen35.c` `f32data()` — F32 norm/conv/SSM tensors, 9 call sites

But fixing all three still produced garbage. The actual culprit was in
`q_fp16_to_fp32()`, a function the audit had explicitly cleared as
"pure integer bit math, endianness-neutral":

```c
unsigned long bits;        /* 8 bytes on LP64 */
...
memcpy(&out, &bits, 4);    /* copies the HIGH half on big-endian */
```

It is not a byte-order bug at all — it is a **type-width** bug that only
becomes visible on big-endian. On a 64-bit big-endian host every fp16
value decoded to `0.0`, so every quantisation scale in the model was
zero. The file parsed, the tensors loaded, nothing errored, and the
output was noise. Fixed by narrowing to a 32-bit object before the copy.

### Cost: none

| | 1.10.0 | 1.11.0 |
|---|---|---|
| end-to-end, x86-64 i8 | 11.96 s | 12.00 s |

0.35%, i.e. noise. The hot path never changed: the k-quant kernels
already read weights a byte at a time, and `rd_f16p` — the most-called
wide read in the program — was already `p[0] | (p[1] << 8)`. F32 data is
0.07% of the weights in a k-quant model.

The i486 constraint had accidentally bought most of this: a 486 has no
wide loads worth using, so the expensive code was byte-wise already.

One near-miss worth recording: the first fix extracted the four bytes by
hand, which was correct but stopped the compiler folding the copy into a
single `movd` and cost **4% end-to-end**. The final version keeps the
one-instruction codegen on both byte orders.

### Also

* `INFER_BIG_ENDIAN` autodetected in `infer.h`, overridable with
  `-DINFER_BIG_ENDIAN=0/1`
* `inf_rd_f32p` / `inf_rd_f64p` / `inf_rd_f32v` in `util.c`
* `gguf_f32data()` — returns the mapping directly on little-endian; on
  big-endian makes one swapped copy per F32 tensor at first use (1.9 MB
  for this model), because the blob is mapped read-only
* `tests/t_endian.c` — 13 assertions, host-independent
* CI cross-compiles for s390x and runs the endian test under qemu

No engine, kernel, tokeniser or MMX code changed. `t_engine` output is
identical and the Geode is unaffected.

---

## 1.10.0 — one think switch, opt-in logging, chat timings

### `--think [on|off]` replaces `--think` / `--no-think`

Two flags for one boolean meant both could be passed at once, with the
outcome decided by argument order. Now there is one switch: `--think`
alone means on, `--think off` is the explicit form.

The value is optional, and the parser only consumes the next word when
it really is one — `--think --raw` sets thinking and leaves `--raw`
alone. A bad value names itself instead of blaming the wrong word:

```
$ infer chat model.gguf --think banana
'--think' expects on or off, not 'banana'
```

`/think on|off` in the REPL is unchanged.

### Logging is opt-in again, including in profile builds

**Fixed:** `infer-profile.exe` printed a full stage profile and a perf
block after every request whether or not they were asked for. `main.c`
had `prof_perf_enabled = log_perf || PROF_ENABLED`, so compiling with
`-DINFER_PROFILE` forced reporting on. It has behaved this way since
1.2.0, when the profile builds were introduced.

Building for measurement and asking for measurements are now separate:

```
--log-perf      TTFT, tokens/second, seconds/token per request
--log-stages    the per-stage profile at exit
```

A `-DINFER_PROFILE` build is silent until one of them is given. Normal
builds still contain no stage timers at all, and `--log-stages` there
says so rather than doing nothing:

```
$ infer run model.gguf --log-stages
--log-stages needs a build with -DINFER_PROFILE (make profile /
profile-geode / profile-windows); this build has no stage timers
compiled in
```

### `chat` reports speed

`chat` had no timing instrumentation at all. With `--log-perf` it now
prints tokens/second and seconds/token after every turn, and a session
total on exit:

```
--- perf: chat session (2 turns) ---
  prompt      :    48 tok in 14.54 s   (3.302 tok/s, 0.30 s/tok)
  generation  :    16 tok in 6.91 s    (2.316 tok/s, 0.43 s/tok)
  total       : 21.45 s
```

`/perf` shows the running total mid-conversation, and `/set` now lists
whether logging is on. Because every turn re-ingests the conversation,
the per-turn prompt figure grows with history — visible evidence for
when `/forget` is worth using.

### Also

* Fixed a 48-byte leak in the Jinja subscript evaluator
  (`jinja_eval.c`), found with AddressSanitizer. It allocated a `none`
  fallback eagerly and overwrote it on every successful lookup.
  Pre-existing, one allocation per process, not per turn.
* `t_agent` grew to 18 assertions covering `--think` semantics and the
  opt-in logging switches; CI checks that `--no-think` is gone and that
  a profile build stays quiet.

---

## 1.9.0 — options are general, not per mode

Every toggle now works in every mode where it can mean something.
`--system`, `--think`, `--no-think`, `--raw`, `--template`, `--mcp`,
`--tool-rounds` and `--quiet-tools` apply to `serve`, `chat` **and**
`run` alike; previously they were parsed everywhere but read only by
`chat`, so using them elsewhere did nothing and said nothing.

### One option table (`src/opts.c`)

Each option is declared once, with the set of modes it applies to. That
declaration drives parsing, the generated `--help`, and an applicability
check, so the three can no longer disagree.

```
$ infer run model.gguf --port 9090
'--port' has no effect in run mode (it applies to: serve)
```

Only five options are restricted, each for a concrete reason:
`--host`, `--port`, `--api-key`, `--alias` (nothing else opens a socket)
and `-p/--prompt` (`serve` takes its prompt from the request body).

### Shared prompt and tool layer (`src/agent.c`)

Prompt rendering and tool calling moved out of `chat.c`. All three modes
call the same code, so they cannot drift; a test asserts that a
conversation built by `chat` and the same one arriving as JSON at
`serve` render to byte-identical prompts.

### What each mode gained

**`run`** — `--system`, `--think`, `--template`, `--mcp` and the full
tool loop. It renders the real chat template instead of the hand-written
ChatML string it used before.

**`chat`** — `-p` seeds the first turn before the REPL opens; `--raw`,
`--tool-rounds`, `--quiet-tools`; `/set` and `/raw on|off` commands.

**`serve`** — `--system` and `--think` as per-request defaults,
`--raw`, and `--mcp` with a **server-side tool loop**: the server
executes tools itself and returns only the final answer, so an ordinary
OpenAI client gets tool use without knowing it. A request carrying its
own `"tools"` array takes over instead. Requests may override with
`"raw"`, `"think"` and `"enable_thinking"`.

### Two bugs found while testing

**JSON-form tool calls were parsed as empty.** Qwen3.5 emits tool calls
in two shapes; only the XML one was handled. The JSON form still
"matched", found no `<parameter=` tags, and produced an empty argument
object — so the tool ran with no arguments and returned a
plausible-looking wrong answer, with nothing in the logs to indicate a
parse failure. Both forms, the OpenAI `{"function":{…}}` nesting and
double-encoded arguments are now handled and tested.

**The server discarded most of each message.** Only `role` and
`content` were copied out of incoming messages, so templates reading
`message.tool_calls` or `message.reasoning_content` silently lost them
and tool-using conversations could not be replayed through the API.
Whole messages are now converted.

### Also

* `--no-think` to switch thinking off explicitly
* `--tool-rounds <n>` (default 4) and `--quiet-tools`
* `tests/t_agent.c` — 10 assertions covering cross-mode prompt
  equivalence and every tool-call shape
* CI asserts the option matrix: every general flag accepted in every
  mode, and inapplicable ones rejected rather than ignored

No engine, kernel or tokeniser code was touched; `t_engine` output is
unchanged and performance is unaffected.

---

## 1.8.0 — custom templates, and full documentation

### `--template <file>`

Load a Jinja chat template from disk instead of the one baked into the
GGUF. Works for both `chat` and `serve`.

```sh
infer chat model.gguf --template qwen-fixed.jinja
```

Verified against
[froggeric/Qwen-Fixed-Chat-Templates](https://huggingface.co/froggeric/Qwen-Fixed-Chat-Templates)
— a 16 kB community template, twice the size of the built-in one.
Rendering it with `infer` and with Python Jinja2 produces
**byte-identical** output for plain chat, for tool-enabled prompts, and
with thinking on and off.

Three Jinja features were added to support it:

* **`|join(sep)`** filter (also `|int`, `|abs`)
* **general slicing** `seq[a:b]` with negative and omitted bounds — the
  engine previously handled only `[::-1]`
* **`for k, v in d.items()`** two-variable unpacking over pair lists

### The server now renders Jinja too

`/v1/chat/completions` previously hand-wrote the ChatML layout in C —
correct for Qwen3.5, wrong for anything else. It now renders the model's
own template, or a `--template` override, exactly as `infer chat` does.
Both paths produce identical prompts.

### Documentation

* **`USER-GUIDE.md`** — every command and flag with worked examples
* **`COMPILE.md`** — rewritten as atomic steps for every target,
  including verification commands for 486-purity, XP DLL imports and
  MMX constant alignment
* **`docs/ARCHITECTURE.md`** — data flow, layering, ownership rules
* **`docs/FINDINGS.md`** — 15 findings and oddities, each with the
  measurement behind it
* **`docs/TEMPLATES.md`** — supported Jinja, and how to debug a template
* **`docs/files/`** — one document per source file, 20 in total
* `docs/PERFORMANCE-ANALYSIS.md` moved under `docs/`
* `imgs/example.jpg` linked from the README

---

## 1.7.0 — chat, real Jinja templates, MCP tools

### `infer chat`

An interactive multi-turn REPL. Keeps the conversation, re-renders it
through the model's own template each turn, streams the reply. Commands
for `/history`, `/system`, `/forget`, `/think`, `/tools`, `/prompt` and
`/stats`. See **CHAT.md**.

### Real Jinja2 template rendering

Previously the ChatML layout was hand-written in C — correct for
Qwen3.5, wrong for anything else. There is now a Jinja subset
interpreter (`jinja.c`, `jinja_eval.c`, ~1200 lines of C89) that renders
`tokenizer.chat_template` directly from the GGUF.

Covers `if`/`elif`/`else`, `for` with the `loop` object, `set` (inline
and block), `macro` with defaults and kwargs, `namespace()`, 11 filters,
9 tests, the full operator set including ternaries, string methods,
`[::-1]` slicing and `raise_exception()`.

**Verified byte-identical to Python Jinja2** on the real Qwen3.5
template — plain chat and tool-enabled, thinking on and off.

Two bugs found and fixed while getting there, both of which silently
corrupt prompts:

* `{{-` was trimming whitespace that an *expression* had just emitted,
  and reaching back into the previous loop iteration. It must only trim
  literal template text, and never past the start of the current
  iteration. This was deleting the newline after every chat message.
* `|tojson` must **sort object keys** (Jinja2 uses
  `json.dumps(sort_keys=True)`). Without it the tools prompt differed
  byte-for-byte from every other runtime.

### MCP tool support over HTTP

`--mcp http://host:port/path` connects to an MCP server using the
Streamable HTTP transport: `initialize`, `notifications/initialized`,
`tools/list`, `tools/call` over JSON-RPC 2.0. Discovered tools are fed
into the template's `tools` variable, and tool calls are executed and
returned to the model for up to 4 rounds per turn.

Plain-JSON and SSE responses are both handled, as is `Mcp-Session-Id`.

stdio transport is **not** implemented (no portable ANSI C way to spawn
children and wire pipes). https is **not** supported (no TLS stack).

`net.h` gained one function, `net_connect()`, implemented for both POSIX
and Win32 using `gethostbyname()` rather than `getaddrinfo()` so it
still works on Windows XP and earlier.

### Verification

* `t_jinja`: 25 expression/statement cases plus the real template from a
  model file; byte-compared against Python Jinja2 offline.
* Tool-call parser unit-tested against 6 shapes including a missing
  `<tool_call>` wrapper and duplicate parameter names.
* Multi-turn memory confirmed end-to-end; MCP tool call confirmed
  end-to-end against a test server.
* 9 build targets, zero warnings; `i486` still emits no MMX; both `.exe`
  still 3 DLLs with no post-XP API; `run` and `serve` unchanged.

---

## 1.6.0 — the recurrence, and a reverted regression

### What 1.5.0's data actually said

The 1.5.0 Geode log contained a sharp signal that was not expected:

| format | instrs cut | time cut |
|---|---|---|
| Q5_K | 52 -> 26 (50%) | **21%** |
| Q4_K | 20 -> 16 (20%) | **1%** |

Both got instruction reductions; only one got faster. The difference is
*which* instructions. Q5_K's cut removed re-**loads** (it was re-reading
and re-widening the same weight bytes four times per chunk). Q4_K's cut
removed register-only ALU ops.

Dividing time by memory accesses gives the same answer for every format:

```
Q4_K  8.0 cyc/MAC -> 25.6 cycles per memory access
Q5_K  9.4 cyc/MAC -> 25.1 cycles per memory access
Q6_K  9.8 cyc/MAC -> 26.1 cycles per memory access
```

~25 cycles per access, flat across formats with very different ALU
loads. **The kernels are bound by memory accesses, not arithmetic.**

### Multi-row: tried, measured, rejected

The obvious response is to reuse each activation load across several
output rows — the matvec re-reads the whole activation vector once per
output row.

I built a correct 2-row Q4_K kernel (verified bit-identical to the
1-row version) and benchmarked it against a realistic streaming weight
buffer. It came out **0.90x — slower.**

The reason is register pressure. MMX has 8 registers; with `mm7` as the
zero constant and four accumulators (2 rows x lo/hi nibble), only three
remain. That is not enough to keep both rows' unpacked nibbles live, so
the 2-row kernel re-loads each weight byte four times. It trades 4 saved
activation loads for 6 extra weight loads.

Recorded here so nobody tries it again: **multi-row is an SSE2
optimisation (16 registers, 8 lanes), not an MMX one.**

### What did work: fusing the delta-net recurrence

With matvec at 91% of runtime it is easy to ignore the rest, but the
state recurrence had grown to **5.7%** and had never been touched.

Per head, `S` is 128x128 floats = **64 KB — exactly the Geode's L1 data
cache**. The code made four separate full passes over it (decay, delta,
rank-1 update, readout), so S was streamed through L1 four times per
head, 16 heads x 18 layers per token.

Passes 1+2 are now fused (decay a row, then immediately dot it with k)
and passes 3+4 are fused (update a row, then dot it with q). Each row is
touched while still hot; S traffic halves.

Measured on the reference host: **0.290 s -> 0.191 s, 1.52x** on that
stage. Cache effects are understated on a machine with a 32 MB L3, so
the Geode should do at least as well.

The arithmetic is unchanged and the output is **bit-identical** to
1.5.0, verified over a 40-token greedy generation.

### Reverted: i8 Q4_K lookup tables

1.5.0 added nibble lookup tables to the portable backend on the strength
of a 2.46x isolated microbenchmark. The Geode i8 log shows what they
actually did:

| | 1.4.0 | 1.5.0 |
|---|---|---|
| Q4_K | 313.3 s | **322.5 s** (3% *worse*) |
| Q5_K | 239.0 s | 236.8 s |
| Q6_K | 233.0 s | 171.4 s |

Q4_K got slower. The tables are now used only by Q5_K, whose inner loop
already loads the `qh` byte so the extra table load rides along with a
load that was happening anyway.

Lesson recorded in the source comment: **a microbenchmark with
everything in L1 does not predict this machine.** It cost a release to
learn.

### Verification

* 2,244 comparisons (320 tensors x 6 activation patterns x 2 backends)
  against the scalar reference: zero failures, worst relative L2
  1.219e-02, unchanged.
* Output **bit-identical to 1.5.0** across a 40-token greedy sample —
  important, because the recurrence change touches model maths, not just
  a kernel.
* 9 build targets, zero warnings; `qwen35.c` and `backend.c` both still
  strict C89; `i486` emits no MMX; all three `.exe` still 3 DLLs with no
  post-XP API and `.rdata` 8-byte aligned.

### Where this leaves things

Per forward pass on the Geode (1.5.0 figures):

| | share |
|---|---|
| matvec (all formats) | 91.1% |
| state recurrence | 5.7% -> now ~3.8% |
| causal conv + SiLU | 1.5% |
| everything else | 1.7% |

The matvec kernels are at ~25 cycles per memory access and ~3.2 cycles
per instruction against a documented ~2-cycle pipeline throughput. There
is no large arithmetic win left in MMX. What remains:

1. **A smaller model or quantisation** — the only lever with multiples
   left in it. Q4_K_S, or a 0.4B-class model.
2. **Batched prompt ingestion** — TTFT only, but large (3.1 min now).
3. **Requantising `token_embd`** — the LM head is 18.1% in 11 calls.

---

## 1.5.0 — fewer instructions per multiply

### What 1.4.0 taught us (negative result)

1.4.0 batched `EMMS` 16x and rescheduled Q4_K to break dependency
chains. On the Geode that bought **1.04x** — I had predicted "1.1x or
much more". That is a useful negative result: **EMMS and dependency
stalls were not the dominant cost.**

Disassembling the 32-bit build showed why. In the hot loop:

```
pmaddwd  :  56   <- the only instruction doing real work
movq     : 231   <- 4.1 data moves per multiply
pand     : 104
psrlw    :  76
psllw+paddw: 96  <- Q5_K hi-bit insertion
punpck   :  64
MMX total: 730 for 56 pmaddwd  = 13 instructions per multiply
```

Working back from the Geode's own numbers: ~1936 M MMX instructions per
forward pass against a 6186 M cycle budget = **3.2 cycles per MMX
instruction**, versus OLPC's measured pipeline throughput of ~2. The
chip was already close to its issue limit. It was not stalling — it was
being asked to execute far too many instructions.

So 1.5.0 attacks instruction count, not scheduling.

### Byte-domain nibble masking (MMX)

The kernels widened bytes to 16-bit words *first* and then masked each
half separately. Masking in the **byte domain** before widening lets one
`pand` handle all eight nibbles at once:

| | before | after |
|---|---|---|
| Q4_K, per 16 weights | 20 instr | 16 instr |
| Q5_K, per 16 weights | 52 instr | 26 instr |

Q5_K was the worst offender — it re-loaded `qs`, re-widened it, and
redid the hi-bit shift/mask/shift for every one of its four `pmaddwd`.

Measured kernel throughput (32-bit build, Mflop/s):

| format | 1.3.0 | 1.4.0 | 1.5.0 |
|---|---|---|---|
| Q4_K | 4279 | 5125 | **5425** |
| Q5_K | 2847 | 2847 | **3953** |
| Q6_K | 2734 | 3206 | 3206 |
| Q8_0 | 2451 | 2591 | 2591 |

Q6_K was left alone deliberately: its values carry a −32 bias, which
makes them signed, and `punpcklbw` against a zero register
zero-extends. Byte-domain masking there needs careful sign handling and
the risk of a silent numeric error outweighed the ~25% of runtime it
represents.

### Nibble lookup tables (portable i8 backend)

Two 256-byte tables map a packed byte to its low and high nibble,
replacing an AND and a SHIFT per weight with two loads. 512 bytes total
— permanently resident in even a 486's 8 KB L1.

On an isolated 486-targeted benchmark this is **2.46x** on the Q4_K
inner loop. In the full kernel the gain is smaller but real (Q4_K and
Q5_K both improve); Q6_K measured *slower* with tables, so it keeps the
arithmetic path. This needs no SIMD and helps the plain `i486` build.

### Measured end-to-end

Best-of-2, 32-bit builds, on an out-of-order reference host:

| build | 1.4.0 | 1.5.0 |
|---|---|---|
| i486 / i8 | 22.6 s | 21.3 s |
| geode / mmx | 9.1 s | 8.4 s |

**Caveat as always:** the reference host is out-of-order with a fast
ALU and a large L1, which systematically under-reports changes aimed at
in-order hardware. Track record: 1.3.0 measured 1.72x here and **2.08x**
on the Geode. Run-to-run noise on this shared machine is ±15%, larger
than several of the deltas above, which is why the per-kernel table is
the more trustworthy measurement.

### About branch prediction

It was suggested the Geode LX has no branch predictor. **It does**, and
two independent measured sources confirm it:

* OLPC, benchmarking real XO-1 silicon: *"The IU has a branch predictor
  of an unspecified size"*
* 7-cpu.com, measured on a Geode LX 800: *"Branch misprediction penalty
  = 8 cycles"*

A misprediction penalty cannot exist without a predictor. And the
arithmetic bounds it regardless: the matvec loops execute ~56 M branches
per forward pass, so even at an implausible 25% miss rate at 8 cycles
that is **1.8% of the cycle budget**. Branches are not where the time
goes — 13 MMX instructions per multiply was.

### Verification

* 2,244 comparisons (320 tensors x 6 activation patterns x 2 backends)
  against the scalar reference: **zero failures**, worst relative L2
  1.219e-02, unchanged from 1.3.0.
* All four build/backend combinations still produce "The capital of
  France is **Paris**."
* 9 build targets, zero warnings; `backend.c` still strict C89;
  `i486` still emits no MMX; all three `.exe` still 3 DLLs, no post-XP
  API.

---

## 1.4.0 — fewer FPU stalls

Two changes, both aimed at the finding from the 1.3.0 Geode profile:
the kernel was running at **~9 cycles per MMX instruction** where the
Geode's documented pipeline throughput is ~2. It was latency-bound, not
work-bound.

### 1. One `EMMS` per row instead of one per 64 weights

`EMMS` switches the FPU between MMX and x87 state. OLPC's Geode notes
warn that "every data exchange requires synchronization, which can
consume a LOT of cycles" — the LX's FPU is asynchronous to the integer
unit.

The 1.3.0 kernels issued an `EMMS` after every 64-weight sub-row, which
for this model works out at roughly **8 million `EMMS` per forward
pass**. Measured on the reference host, wrapping a small MMX block in
`EMMS` costs ~3.6x the block's own work.

Every per-format kernel is now split into two phases: all MMX blocks for
a whole matrix row run first, accumulating raw integer dot products into
a scratch array; then a *single* `mmx_sync()`; then all the float scale
arithmetic. That takes the count from ~8.05M to ~0.50M per pass — **16x
fewer FPU state switches**.

### 2. Dependency-friendly instruction scheduling (Q4_K)

The Q4_K chunk funnelled all four `pmaddwd` results through one scratch
register and accumulated two of them into the same register back to
back, so nearly every instruction waited on its predecessor.

Now the two `punpck` halves, the four masked vectors and the four
`pmaddwd` destinations are all distinct registers, and the four `paddd`
alternate between two accumulators. A microbenchmark of the serial
versus interleaved shape measured **1.97x** on a fully serial chain.

Q4_K throughput on the 32-bit build: **4071 → 5125 Mflop/s**.

Q5_K and Q6_K were left alone. Q5_K in particular needs two more live
temporaries than MMX's eight registers allow once two are accumulators;
forcing it would spill and lose more than it gains. If you want to push
further, that is where the remaining headroom is.

### Measured

32-bit `geode` build, same prompt, greedy:

| backend | 1.3.0 | 1.4.0 | gain |
|---|---|---|---|
| `mmx` | 10.4 s | 9.3 s | **1.11x** |

**Caveat, stated plainly:** this was measured on an out-of-order x86,
which hides exactly the stalls these changes remove. The real gain on
the in-order Geode is unknown from here and could be anywhere from
~1.1x to substantially more — the 1.3.0 kernels showed 1.72x on this
host and **2.08x** on the Geode, so the development host consistently
under-reports. Only the hardware can answer it.

### Verification

* **2,244 comparisons** — all 320 tensors x 6 activation patterns x both
  fast backends against the scalar reference. Zero failures, worst
  relative L2 1.219e-02, byte-identical to 1.3.0.
* All three backends still produce "The capital of France is
  **Paris**."
* 11 build targets, zero warnings; `i486` still emits no MMX; all three
  `.exe` still import only KERNEL32/msvcrt/WS2_32 with no post-XP API;
  PE `.rdata` still 8-byte aligned.

### Where the remaining time goes

From the 1.3.0 Geode profile, per forward pass (12.85 s):

| | share |
|---|---|
| feed-forward | 36.8% |
| delta-net input projections | 26.7% |
| lm head | 16.1% (11 calls, 5.5 s each) |
| delta-net output projection | 7.7% |
| attention | 6.1% |
| state recurrence | 5.1% |

At ~594 MMAC per token and 4 MACs per `PMADDWD`, pure instruction issue
at the documented 2-cycle throughput would be ~2.8 s per pass. We are at
12.85 s. The gap is stalls, which is why scheduling — not more
arithmetic — is the lever that remains.

Honest assessment of the 5 s/token target: it needs ~3.4x from here.
That is at the edge of what the hardware allows, and getting there would
require the Q5_K/Q6_K kernels rescheduled as carefully as Q4_K now is,
plus probably a smaller quantisation. Not impossible; not close to
guaranteed.

---

## 1.3.0 — faster MMX kernels, and a real alignment bug fixed

**Summary: `mmx` is ~1.7x faster end-to-end, `i8` ~1.1x, output
byte-identical.** The kernel rewrite came from an independent
contributor; this release integrates it after an audit that found and
fixed a latent crash.

### The bug that had to be fixed first

The contributed `backend_mmx.c` declared its MMX constants as bare
`short[4]`:

```c
static const short c_mask_f[4] = { 0x000F, 0x000F, 0x000F, 0x000F };
```

and loaded them with `movq`, **which requires an 8-byte-aligned source
operand**. A `short[4]` is only 2-byte aligned per the C standard, and
GCC gives `.rodata` 4-byte alignment on 32-bit x86. Verified against the
actual object file: **all five constants landed at addresses ≡ 4 (mod
8) — every one misaligned.**

An out-of-order x86 splits a misaligned MMX load transparently, so this
is completely invisible on a modern development machine. It is a
CPU-specific courtesy, not an architectural guarantee: it costs a
penalty on every access and can fault outright on strict, emulated or
virtualised implementations. It was reported as a hard failure on real
Geode LX and Windows XP hardware.

Note the contributor's report did state the disassembly had been audited
and was clean — and it was, for *instruction set*. Data alignment is a
separate axis that nobody checked.

**Fix:** an explicit `__attribute__((aligned(8)))` on each constant.

A `union` with `long long` is **not** sufficient — the i386 SysV ABI
aligns `long long` to 4, not 8, so GCC emits no `.align 8` and the
constants stay misaligned. That was tried first and rejected after
checking with `readelf`.

Measured effect on the emitted objects:

| | ELF `.rodata` | PE `.rdata` |
|---|---|---|
| before | align 4 — **broken** | `2**2` — **broken** |
| after  | align 8 — correct | `2**3` — correct |

`tests/t_align.c` now asserts at run time that all five constants are
8-byte aligned, so a compiler that ignores the attribute is caught by
`make test` instead of by a crash on the target. `backend_mmx.c` also
now `#error`s on non-GCC compilers rather than silently emitting
misaligned data.

### Kernel improvements (contributed, verified, kept)

* **In-register nibble unpacking.** Weights are expanded straight into
  MMX registers with `PUNPCKLBW`/`PUNPCKHBW` + `PAND`/`PSRLW` instead of
  a scalar loop writing to a staging buffer. The old code accelerated
  only the multiply and left the unpack — the more expensive half —
  scalar, plus 256 bytes per group of pointless store/reload traffic.
* **Real MMX Q6_K kernel.** Previously Q6_K fell back to scalar code
  entirely, and Q6_K is ~30% of matvec time (it carries the LM head).
  The −32 bias is folded into the weights with `PSUBW`.
* **MMX Q8_0 and Q4_0 kernels** (previously scalar).
* **Batched `EMMS`** — one per sub-row (Q4_K/Q5_K) or per half-block
  (Q6_K) instead of one per 32 lanes.
* **Precomputed per-16 activation sums** (`bk_qx.s16[]`) for the Q6_K
  bias term, instead of re-summing 16 elements eight times per block.
* **Branch-free Q5_K hi-bit**: `((h >> sh) & 1) << 4` rather than
  `(h & u) ? 16 : 0`.

The last two also speed up the portable `i8` backend, which needs no
MMX and still runs on a 486.

### Measured

32-bit `geode` build, 19-token prompt + 8 tokens, greedy:

| backend | 1.2.0 | 1.3.0 | speedup |
|---|---|---|---|
| `i8`  | 20.0 s | 17.9 s | **1.12x** |
| `mmx` | 18.0 s | 10.5 s | **1.72x** |

Per-kernel throughput, same build (Mflop/s):

| format | `ref` | `i8` | `mmx` |
|---|---|---|---|
| Q4_K | 1462 | 2085 | **4279** |
| Q5_K | 1306 | 1667 | **2847** |
| Q6_K | 335  | 2016 | **2734** |
| Q8_0 | 2264 | 1665 | **2451** |

### Verification

* **2,244 comparisons** — every one of the model's 320 tensors × 6
  activation patterns (uniform, large, tiny, sparse, alternating,
  all-zero) × both fast backends, each against the scalar reference.
  **Zero failures**; worst relative L2 error 1.2e-2, consistent with
  int8 activation quantisation and unchanged from 1.2.0.
* All six build/backend combinations produce byte-identical output.
* `i486` target still emits **zero** MMX instructions.
* `backend.c` still compiles clean under `-std=c89 -pedantic -Wall
  -Wextra`.
* All three `.exe` still import only KERNEL32, msvcrt and WS2_32, with
  no post-XP API.

### Rejected

The contributed report recommended adding 3DNow! `PREFETCH` on the
grounds that the workload is memory-bound. **Profiling on real Geode
hardware shows it is not:** 14 MB/s of ~390 MB/s available (3.6%
utilisation) and 22.5 cycles per MAC. The workload is compute-bound and
prefetching would gain approximately nothing. That recommendation
originated from an outdated note in this project's own README, since
corrected.

### Other

* Version string bumped to 1.3.0 (the profiling logs in 1.2.0 still
  reported 1.0.0).
* `make test` now includes `t_align`.

---

## 1.2.0 — profiling

* Compile-time stage profiling (`-DINFER_PROFILE`, `make profile*`),
  zero overhead when not enabled.
* Always-available throughput logging (`--log-perf`): TTFT, prompt and
  generation tokens/second.
* `--log-file` to write either to a file.
* High-resolution monotonic clock in the platform layer
  (`gettimeofday` / `QueryPerformanceCounter`).

## 1.1.0 — selectable compute backends

* `ref` (scalar float, 486-safe), `i8` (integer, 486-safe), `mmx`.
* Run-time selection via `--backend`, CPUID probing, `--backend list`.
* `make geode`, `make windows-mmx`.

## 1.0.0 — initial release

* GGUF loader, `qwen35` hybrid engine, byte-level BPE tokeniser,
  OpenAI-compatible HTTP server.
* ANSI C (C89), no dependencies, POSIX and Windows XP backends.
