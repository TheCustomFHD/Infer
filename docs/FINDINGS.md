# Key findings and oddities

Things that were surprising, cost real debugging time, or would bite the
next person. Every claim here is backed by a measurement.

---

## 1. Qwen3.5 is not a transformer

`general.architecture = "qwen35"` is a **hybrid**. Of its 24 blocks:

* **6** (indices 3, 7, 11, 15, 19, 23) are gated grouped-query attention
* **18** are **Gated DeltaNet** linear-attention layers carrying a
  128×128 recurrent matrix state per head — no KV cache

A generic llama.cpp-style decode loop cannot run this model. This was
established by reading the GGUF metadata *before* writing any code, and
cross-checked against llama.cpp's reference graph.

Pleasant consequence: DeltaNet layers cost the same per token regardless
of conversation length. Only the six attention layers grow with context.

---

## 2. "Q4_K_M" is not a Q4_K file

Byte composition of `Qwen3.5-0.8B-Q4_K_M.gguf`:

| format | share |
|---|---|
| **Q6_K** | **46.9%** |
| Q4_K | 32.7% |
| Q5_K | 19.9% |
| Q8_0 | 0.1% |

The suffix names the *dominant* format, not the only one. k-quants
assign a different format per tensor by sensitivity. A "Q4_K-only" build
would refuse to load this file.

`token_embd` alone — Q6_K, tied embedding and LM head — is **199 MB,
40% of the file**. Every k-quant mixture, including Q3_K_S, keeps it at
Q6_K.

---

## 3. Q4_0 is the same size as Q4_K, and worse

Both are **4.5 bits per weight**: Q4_0 is 18 bytes per 32 weights, Q4_K
is 144 per 256. Q4_0 spends those bits worse — symmetric only, no
per-sub-block minimum. Same size, lower quality. Its only advantage is a
simpler decode.

---

## 4. The MMX alignment bug — invisible on a dev machine

Contributed MMX kernels declared their constants as `short[4]` and loaded
them with `movq`, **which requires 8-byte alignment**. A `short[4]` is
2-byte aligned per the standard, and GCC gives `.rodata` 4-byte
alignment on 32-bit x86.

Verified against the object file: **all five constants at offset ≡ 4
(mod 8). Every one misaligned.**

Out-of-order x86 splits a misaligned MMX load transparently — so this
worked perfectly on the development machine and **failed on real Geode
LX / Windows XP hardware**.

A `union` with `long long` is **not** enough: the i386 SysV ABI aligns
`long long` to 4. Verified with `readelf` after trying it. The fix is an
explicit `__attribute__((aligned(8)))`.

| | ELF `.rodata` | PE `.rdata` |
|---|---|---|
| before | align 4 | `2**2` |
| after | align 8 | `2**3` |

`tests/t_align.c` now asserts it at run time. **The contributed report
had audited the disassembly and found it clean — for instruction set.
Data alignment is a separate axis nobody checked.**

---

## 5. MMX registers alias the x87 stack — twice over

**`EMMS` is required** before any float instruction, or you read garbage.

**GCC will reschedule float work into an asm block** unless told not to.
At `-O2` this produced silent NaNs until `"st"`–`"st(7)"` were added to
the clobber list. At `-O0` it was correct — a classic
"works-in-debug" trap.

**`EMMS` is also expensive.** OLPC measured that on the Geode "every
data exchange requires synchronization, which can consume a LOT of
cycles". The kernels originally issued one per 64 weights — ~8 million
per forward pass. Batching to one per row cut that to ~0.5M.

---

## 6. The Geode's fast L1 window is 4 KB, not 64 KB

> "It adds two clocks of cache latency if the memory operand is not in
> last accessed way of data cache. Last way of L1 covers 4 KB."
> — 7-cpu.com, measured

The L1 is 64 KB / 16-way, but only the last-accessed way is fast.

This explains a failed optimisation. The delta-net state `S` is 64 KB per
head, and the code made four passes over it. Fusing to two passes
measured **1.52× on the dev host** and **1.002× on the Geode** — because
94% of `S` was already outside the fast window regardless of pass count.

---

## 7. The Geode is compute-bound, not memory-bound

An early README claimed the opposite and recommended prefetching. The
profile disproved it:

| | |
|---|---|
| bandwidth used | **14 MB/s of ~390 MB/s (3.6%)** |
| cycles per MAC | **22.5** |

Because the model is mmap'd, each weight is read from RAM once per
forward pass and reused from cache. Prefetching would gain approximately
nothing. That correction came directly from a profile log off the target.

---

## 8. `IMUL` is ~16 cycles and is 65% of the `i8` backend

Not published anywhere; derived from the measured log. The inner loop is
14 instructions producing 2 MACs, two of them `IMUL`:

```
21.8 cycles/MAC measured
 6   cycles of non-IMUL work at 1 IPC
--------------------------------------
~16 cycles per IMUL
```

Four attempts to remove it were all rejected — see
[../PERFORMANCE-ANALYSIS.md](PERFORMANCE-ANALYSIS.md). The kernel is
at its floor.

---

## 9. The Geode *does* have branch prediction

It was suggested it did not. Two measured sources say otherwise:

* OLPC, on real XO-1 silicon: *"The IU has a branch predictor of an
  unspecified size"*
* 7-cpu.com: *"Branch misprediction penalty = 8 cycles"*

A misprediction penalty cannot exist without a predictor. And the
arithmetic bounds it anyway: ~56M branches per forward pass, so even at
an implausible 25% miss rate that is **1.8% of the cycle budget**. CMOV
has nothing to work on — the only branch in the hot loop is a loop
back-edge, taken 31 times out of 32.

---

## 10. Microbenchmarks lie about this machine, systematically

| change | dev host | Geode |
|---|---|---|
| contributed kernels | 1.72× | **2.08×** (under) |
| i8 lookup tables | 2.46× isolated | **0.97×** (over) |
| recurrence fusion | 1.52× | **1.002×** (over) |

The dev host is out-of-order with a 3-cycle `IMUL`, 32 MB L3 and a large
fast L1. **Only measurements from the target hardware were trustworthy.**
Every performance claim in this project is anchored to a log from the
real machine.

---

## 11. Byte-level BPE breaks UTF-8 in streams

A single multi-byte character is often split across tokens. Emitting a
fragment produces invalid UTF-8 inside a JSON string, which the official
OpenAI SDK **rejects outright** with a `UnicodeDecodeError`.

Both the server and the chat loop buffer output and emit only complete
UTF-8 sequences. Caught by testing against the real SDK.

---

## 12. Two Jinja details that silently corrupt prompts

**`{{-` must trim only literal template text.** Not the output of an
expression, and never past the start of the current loop iteration.
Getting it wrong deleted the newline after **every chat message** —
output that still looked plausible.

**`|tojson` must sort object keys.** Jinja2 uses
`json.dumps(sort_keys=True)`. Without it the tools prompt differs
byte-for-byte from every other runtime.

Both were found by `cmp`-ing against Python Jinja2, not by inspection.
That comparison is the only reliable test for a template engine.

---

## 13. Small models are unreliable at tool use

The MCP plumbing is correct — handshake, discovery, dispatch, result
feedback all verified. But a 0.8B model called the right tool with
roughly the right arguments and then sometimes mis-stated the result.
Worth knowing before blaming the client.

It is also worth ruling out the plumbing first: what looked like the
model mis-stating a result turned out, once, to be finding 16 below.

---

## 14. `\p{N}` matches one digit at a time

The qwen35 pre-tokeniser splits `1234567890` into **ten** tokens. Correct
for this model family, and a common source of "why is my prompt so long".

---

## 15. Windows 95 is closer than expected

The import table is almost Win95-clean. Every Winsock call used is 1.1
vintage; every KERNEL32 call exists in 95. The blocker is `msvcrt.dll`
startup imports (`__set_app_type`, `__p__commode`, `_amsg_exit`), which
are MSVCRT-version-specific — the *C runtime linkage*, not the program.

A real caveat if anyone tries: `MapViewOfFile` on 95 shares a single
2 GB region across all processes and fragments badly. The `malloc`
fallback in `gguf.c` exists for exactly that.

---

## 16. A tool-call parser that "succeeds" with no arguments

Qwen3.5 emits tool calls in two shapes, chosen by the template's
`tool_call_format`: an XML form (`<function=…><parameter=…>`) and a JSON
form (`{"name": …, "arguments": {…}}`). Only the XML one was
implemented.

The failure mode was the bad kind. Given the JSON form, the parser did
not *reject* it — it found the name, found no `<parameter=` tags, and
returned an empty argument object. The tool was then invoked with `{}`:

```
tool call: add {"a":17}      <- b silently missing
tool result: The sum is 17.
```

A refusal would have been obvious. Instead the tool ran, returned a
plausible number, and the model reported it confidently. Nothing in the
logs distinguished this from success.

Two lessons worth keeping: a parser that can partially match should say
so rather than return a partial result; and a tool-calling path is only
tested when the *arguments* are checked, not just the tool name.

---

## 17. Silently ignored options are a correctness bug, not a UX one

`--system`, `--think` and `--mcp` were parsed in every mode but read
only by `chat`. `infer run model.gguf -p "…" --system "Answer in one
word."` therefore produced a normal, plausible, *wrong-length* answer,
with no indication the constraint had been dropped.

This is the same failure shape as the tool-call bug above: the program
appears to work and quietly does something other than what was asked.
The fix was structural rather than a matter of adding reads — one option
table that records which modes each flag applies to, and rejects the
combinations that do not:

```
$ infer run model.gguf --port 9090
'--port' has no effect in run mode (it applies to: serve)
```

Restrictions then have to be *justified* to be written down, which is
why only five survived. The rest were restricted by accident of where
the code happened to live.

---

## 18. The endianness bug was a type-width bug

A careful source audit of every wide read predicted three broken sites
(`rd_f32`/`rd_f64`, `dot_f32`, `f32data`) and explicitly cleared
`q_fp16_to_fp32()` as "pure integer bit math, endianness-neutral".

Fixing all three predicted sites still produced `!!!!!!!!` on s390x. The
real culprit was the cleared function:

```c
unsigned long bits;        /* 4 bytes on i486, 8 on LP64 */
...
memcpy(&out, &bits, 4);
```

On a 64-bit **big-endian** host the first four bytes of an 8-byte
`unsigned long` are the *high* half — always zero here. Every fp16
decoded to 0.0, so every quantisation scale in the model was zero.

Two things worth keeping:

* **It is not really an endianness bug.** It is a width assumption
  (`unsigned long == 4 bytes`) that happens to be invisible until the
  byte order changes. Auditing for "endianness" by grepping for byte
  swaps would never have found it.
* **It failed silently and plausibly.** The GGUF parsed, all 320 tensors
  loaded, metadata printed correctly, the tokeniser worked, generation
  ran at full speed and emitted confident garbage. Nothing errored.

The audit was worth doing — the three sites it found were genuinely
broken — but it could not have replaced running the thing. Installing
`gcc-s390x-linux-gnu` and `qemu-user-static` took about ten seconds and
answered in one run what inspection got wrong.

## 19. Byte-order neutrality was nearly free, for a 486 reason

Porting to big-endian cost **0.35% end-to-end** — noise. The reason is
that the i486 target had already forced the right habits:

* the k-quant kernels only ever touch `unsigned char`, because a 486 has
  no wide loads worth using;
* `rd_f16p`, the most-called wide read in the program, was already
  `p[0] | (p[1] << 8)`;
* GGUF header parsing was already byte-wise with explicit shifts.

Only F32 data needed real work, and F32 is **0.07%** of the weights in a
k-quant model (1.9 MB of 508 MB; 133 tensors of 320). A constraint
adopted for a 500 MHz netbook bought portability to mainframes.

One near-miss: the first fix extracted four bytes by hand. Correct, but
it stopped GCC folding the store into a single `movd` and cost **4%**
end-to-end — ten times the cost of the entire rest of the port. Narrowing
to a `unsigned int` instead keeps the one-instruction codegen on both
byte orders. Byte-wise code is free only when the compiler can prove it
can put the bytes back together.

---

## 20. Byte-order autodetection is compiler-specific, and guessing is worse than failing

1.11.0 made the code byte-order neutral and verified it on s390x and
SPARC — both compiled with **GCC**. A user then built it on a real
UltraSPARC with **Sun Studio 12.3**, and got `!!!!!!!!!!` after eight
minutes of computation.

The code was fine. The *detection* was not:

| | GCC / Clang | Sun Studio |
|---|---|---|
| SPARC macro | `__sparc__` | `__sparc`, `__sparcv9` |
| byte order | `__BYTE_ORDER__` | not defined |
| unprefixed `sparc` | defined | **suppressed by `-Xc`** |

Our `#if` tested `__sparc__` and `__BYTE_ORDER__`. Under `-Xc` on Sun
Studio, *every* condition was false, so the `#else` branch selected
little-endian — on a big-endian machine. The build was clean, the model
loaded, the metadata printed correctly, and the output was noise.

Two changes, and the second matters more:

1. **Test every spelling** — `__sparc`, `__sparcv8`, `__sparcv9`,
   `_BIG_ENDIAN`, `__hppa`, `mc68000`, and so on, with `__BYTE_ORDER__`
   preferred when present.

2. **Refuse to guess.** The `#else` branch is now `#error`, so an
   unrecognised target fails at compile time with instructions rather
   than silently picking one. And `gguf_open()` calls
   `inf_check_byte_order()`, which compares `INFER_BIG_ENDIAN` against a
   runtime probe and decodes a known bit pattern:

```
byte order mismatch: this binary was compiled for a little-endian host
but is running on a big-endian one.
  rebuild with -DINFER_BIG_ENDIAN=1
```

One second, at startup, instead of eight minutes of arithmetic ending in
garbage.

The general lesson: a portability macro that *guesses* converts a build
error into a wrong-answer bug. Wrong answers that arrive confidently and
at full speed are the most expensive kind. If the code cannot tell, it
should say so.

---

## 21. A prompt cache that can never hit, because BPE re-merges

The prefix cache worked in unit tests and on `/v1/completions`, and
never once hit in chat. The diagnostic said it diverged at token 9:
history had `198`, the new prompt had `14200`.

Both decode to the same text. `198` is `"\n"`; `14200` is `"\n\n\n\n"`.

The model *generated* four newlines as four separate tokens, because it
emits one token at a time. When that same text is re-tokenised as part
of the next prompt, byte-level BPE merges it greedily into one. Identical
characters, different token IDs, no cache hit — ever.

The fix is to tokenise the known-shared prefix separately from the new
remainder, so the prefix reproduces exactly the IDs it had last time.

The general shape: **a cache keyed on tokens is keyed on a lossy
encoding of the text.** Anything that re-encodes can silently break it,
and it fails as a permanent miss rather than a wrong answer, so nothing
draws attention to it.

## 22. The cache still cannot help stock-template chat

Even with tokenisation fixed, chat misses — and correctly so. The Qwen
chat template rewrites history between turns: a reply generated as

```
<think>...</think>Hello! How can I help
```

is re-rendered on the *next* turn as

```
Hello! How can I help
```

with the think block stripped. The token sequence for turn N is
therefore not a prefix of turn N+1's, and the recurrent state that
absorbed the think tokens cannot be rewound.

This is a property of the template, not of the cache, and it means the
cache helps `/v1/completions`, agentic append-only loops, and templates
that preserve history — but not stock-template chat. Worth stating
plainly instead of quoting an average speedup that would not
materialise.

---

## 23. A graphics instruction made exact

VIS has no integer multiply-accumulate. Its partitioned multiply is
built for pixel blending and scales the result:

```
fmul8x16(u8 a, s16 b) -> (a * b + 0x80) >> 8
```

Our products are at most 15 x 127 = 1905, so `>> 8` would leave almost
nothing. The instruction looks unusable.

It is not, because the shift can be cancelled on the way in. Pre-shift
the activation by 8 and the scaling exactly undoes itself:

```
fmul8x16(w, x << 8) = (w * (x << 8) + 0x80) >> 8 = w * x
```

with no rounding error at all, provided `x << 8` fits a signed 16-bit
lane — and quantised activations span only [-127, 127], so it does.

Verified exhaustively on the target hardware semantics: 16320
combinations, zero mismatches.

The lesson generalises: an instruction whose *documented* behaviour is
wrong for your problem may still be exactly right after an algebraic
change of variables. Read the definition, not the name.

## 24. A cache that could only ever hit

The VIS backend pre-shifts activations once per matvec. An obvious
optimisation is to skip that when the activation vector has not
changed, keyed on its pointer and length:

```c
if (xs_src == xq->q && xs_n == xq->n) return;
```

`bk_quantize_x()` returns a pointer into a single **static** buffer. So
`xq->q` is the same address on every call while the contents change
underneath. The condition was true every time after the first, and
every matvec but the first used stale activations.

It passed `t_backend`, which calls matvec once per tensor with one
activation vector, and produced fluent-looking nonsense in real
generation.

Two things worth keeping:

* **Caching on a pointer identity requires that the pointer identifies
  the contents.** For a reused scratch buffer it identifies nothing.
* A test that exercises a function *once* cannot detect state that goes
  stale between calls. The bug lived in the gap between a unit test and
  an end-to-end run, which is exactly where caching bugs live.

---

## 25. Two compilers, two type systems, and a test that never ran

The VIS backend shipped in 1.14.0 claiming "dual compiler support". The
Sun Studio half had never been compiled by a Sun Studio. It was written
from memory against an imagined API:

```c
typedef vis_f32 vis_u8x4;      /* no such type */
typedef vis_d64 vis_s16x4;     /* no such type */
```

Sun's `<vis_proto.h>` declares no vector types whatsoever. A VIS
register *is* an FP register, and the intrinsics are declared over the
plain FP types:

```c
double vis_fmul8x16   (float  /*frs1*/, double /*frs2*/);
double vis_fpadd16    (double /*frs1*/, double /*frs2*/);
double vis_fmuld8ulx16(float  /*frs1*/, float  /*frs2*/);
```

A `u8x4` is a `float` you never do float arithmetic on; an `s16x4` is a
`double`. GCC's `vector_size` + `__builtin_vis_*` spelling is a GCC
extension and does not carry over.

The consequence went past the typedefs. `vis_hsum16()` reached lanes
with `v[0]`, `v[1]` — you cannot subscript a `double`, so the helper
needed a separate implementation per compiler, using a union. The
kernels themselves were fine, because they only ever touch the four
`VMUL8X16`/`VADD16`/`VADD32`/`VWIDEN` macros. Keeping the compiler
divergence confined to the type layer is what limited the damage.

Two smaller traps in the same area:

- `vis_to_float()` / `vis_to_double()` / `vis_read_hi()`, which the test
  used, are mediaLib helpers. They may not exist in Studio's copy of the
  header. Unions are C89, work under `-Xc`, and cannot be wrong.
- The Studio branch of the lane-ordering test was stubbed out with the
  comment "the ordering guarantee is architectural, so a single-lane
  check suffices". Once lane access goes through a union, ordering is a
  property of *our code*, not the architecture. It is now checked.

**Why it went unnoticed:** the plain `make solaris` target does not list
`backend_vis.c` and does not define `INFER_HAVE_VIS`, and the file
collapses to a dummy typedef when that macro is absent. So `make
solaris` succeeds whether or not the VIS code is valid — it never sees
it. A successful scalar build says nothing about the VIS build. The
target now prints that it is scalar-only.

**How it is prevented now:** `tests/vis_shim/vis_proto.h` reimplements
Sun's exact signatures in plain C, so the Studio code path can be
compiled and *run* under GCC with `-D__SUNPRO_C`. Both paths are now
checked against the i8 kernel by `tests/t_viskern.c` on synthetic
blocks, needing no model:

```
Q4_K  ncols=3584 rows=4   worst rel err 0.000e+00  ok
Q5_K  ncols=3584 rows=4   worst rel err 0.000e+00  ok
```

Bit-identical, both compilers. Note the reference has to be `qmv_i8`,
not `qmv_ref`: against the exact F32 reference the VIS kernels show
1–12% relative error, which is the cost of int8 activation
quantisation and is present in the i8 backend too. Comparing against
the wrong reference would have sent us debugging a kernel that was
already correct.

What the shim does **not** tell us is anything about Studio's code
generation — GCC inlines the shim's C rather than emitting VIS
instructions. Instruction counts from it are meaningless. Only the real
compiler on the real machine can answer that.

## 26. `CC ?= cc` cannot override make's built-in default

> **Historical.** The `solaris-gcc-*` targets described below were
> removed in 1.21.0 when the target list collapsed to four artifacts;
> the toolchain choice is now `make solaris TOOLCHAIN=gcc`. The
> finding is kept because the underlying trap — `?=` cannot override a
> make built-in — is still live for any variable make predefines.

The `solaris-gcc-*` targets exist so a Solaris user can build with GCC
instead of Sun Studio. They never did. On a Solaris box:

```
gmake solaris-gcc-vis-profile
cc -std=gnu89 -Wall ... -mcpu=ultrasparc -mvis ...
cc: Warning: Option -d=gnu89 passed to ld, if ld is invoked, ignored otherwise
cc: -W option with unknown program all
gmake: *** [Makefile:281: solaris-gcc-vis-profile] Error 1
```

That is Sun Studio being handed GCC's flags. Studio parses `-std=gnu89`
as `-s` followed by `td=gnu89`, and its `-W` means "pass an argument to
a named compilation phase", so `-Wall` becomes "unknown program all".

The Makefile said `CC ?= cc`. Make assigns `CC = cc` as a **built-in
default before reading the Makefile**, so `?=` sees `CC` already set and
does nothing. On Linux `cc` is GCC and the bug is invisible; on Solaris
`cc` is Studio and the GCC targets silently call the wrong compiler.
`CC = gcc` would be no better — it would break every other target for
anyone whose compiler is not named `gcc`.

**Fix:** a separate variable. `GNUCC = gcc`, used *only* by the five
`solaris-gcc-*` targets; `$(CC)` still drives the x86 and generic test
targets, `$(SUNCC)` the Studio ones. Overridable for a non-PATH GCC:

```sh
gmake solaris-gcc-vis GNUCC=/usr/sfw/bin/gcc
```

Verified by putting a stub `cc` that rejects GCC flags the way Studio
does ahead of a real GCC on `PATH`, then confirming target-to-compiler
routing:

```
solaris                    -> cc
solaris-vis                -> cc
solaris-gcc                -> gcc
solaris-gcc-vis            -> gcc
solaris-gcc-vis-profile    -> gcc
```

The same test caught a second bug: `--help` hardcoded `auto, mmx, i8,
ref`, so a VIS build never mentioned `vis`. The string is now
conditional on `INFER_HAVE_VIS`. `--backend list` was always correct,
which is why this survived — the two disagreed and only the less-read
one was right.

**The general lesson:** a build target for a platform you cannot run is
not verified by reading it. A five-line stub compiler on `PATH` tests
the *routing* even when the real toolchain is unavailable, and routing
is where these bugs live.

## 27. A byte-order bug that only a big-endian machine can see

The natural way to load four weight bytes into a VIS register is to
build a 32-bit word and pun it into lanes. The natural way to build
that word is little-endian:

```c
return (unsigned int) (q[0] | (q[1] << 8)) |
       ((unsigned int) (q[2] | (q[3] << 8)) << 16);
```

`PACK_U8X4` reinterprets the word as four u8 lanes, and lane 0 is the
byte the word's memory image *starts* with. On big-endian SPARC that is
the most significant byte, so the loader above delivers the weights
**reversed** while the activations stay in order:

```
bytes in memory : 11 22 33 44
x86    lanes    : 11 22 33 44   correct
sparc  lanes    : 44 33 22 11   reversed
```

The products are then summed, so the error is not a clean permutation
of the output -- it is a wrong dot product, silently. Measured error
against the i8 kernel ran to 8194% on Q4_0.

Two properties made this dangerous:

- It is **invisible on x86**. A shim that emulates VIS arithmetic on a
  little-endian host reports every case bit-exact.
- It only affects formats whose planes are *not* 4-aligned. Q4_K and
  Q5_K super-blocks are 144 and 176 bytes, so a plain `lduw` always
  works and the native load preserves memory order. Q6_K (210), Q8_0
  (34) and Q4_0 (18) have odd strides, need a byte-assembling loader,
  and are the only ones that fail. Passing three formats out of five is
  exactly the pattern that makes a bug look like a corner case.

**The rule this repo already had:** finding 20 says never guess byte
order, `#error` instead. The same rule applies to *validation*: a test
that can only run little-endian cannot certify a big-endian target.
`make vis-shim-test` now says so in its own output, and the loaders are
written so both orders produce identical lane order by construction:

```c
#define VIS_LDW(p)  (*(const unsigned int *) (const void *) (p))

static unsigned int vis_ldw2(const unsigned char *p) {
#if INFER_BIG_ENDIAN
    const unsigned short *h = (const unsigned short *) (const void *) p;
    return ((unsigned int) h[0] << 16) | (unsigned int) h[1];
#else
    return ((unsigned int) p[3] << 24) | ((unsigned int) p[2] << 16) |
           ((unsigned int) p[1] << 8)  |  (unsigned int) p[0];
#endif
}
```

## 28. Three optimisations that measurement rejected

All three were tried on the Q6_K kernel and reverted. Counts are static
instructions per super-block from a real sparc64 cross-GCC at `-O2`,
fully unrolled.

**Normalising alignment with a copy — not adopted.** Q6_K's 210-byte
stride makes every second super-block start 2 mod 4. Copying the
192-byte quant plane into an aligned buffer buys `lduw` loads
throughout: 1423 instructions against 1504 for choosing a loader per
block. But the copy is ~48 extra memory operations and dirties 192
bytes of a 16 KB *direct-mapped write-through* D-cache per super-block,
against 256 weights of actual work. The instruction count favours the
copy by 5%; the cache behaviour is unmeasurable without hardware. Kept
the copy-free version and recorded the 5% as a known, deliberate cost
rather than claiming a win either way.

**64-bit loads — rejected, 1395 -> 1477.** Every load offset in the
kernel is 0 mod 8, so `ldx` could replace pairs of `lduw` and halve the
load count. It made the kernel *worse*: the extra shifting and masking
to split the doubleword cost more than the saved load, and holding
64-bit values raised pressure on the 32-entry FP register file. Halving
memory operations is not the same as halving cost when the operations
were already cheap.

**Rewriting Q4_K/Q5_K around 32-bit accumulation — rejected.** Folding
the 16-bit chains into 32-bit lanes earlier looks like it should reduce
fold traffic. Measured: Q4_K 261 -> 280 instructions (106 -> 118 memory
ops), Q5_K 391 -> 411 (146 -> 170). Both worse, so both originals were
kept unchanged. The existing 16-bit accumulation already runs the
maximum number of terms the lane width allows, and anything that folds
sooner just adds work.

**A note on the accumulator.** Splitting a kernel into aligned and
unaligned variants initially broke bit-exactness (2.4e-07 instead of
0). Each variant started its own `sum = 0.0f`, so partial sums were
rounded per super-block instead of accumulating in one running float.
Threading the accumulator through as a parameter restored exactness.
Bit-identity with `i8` is a property of *float accumulation order*, not
just of the integer arithmetic.

## 29. Two lane budgets that were never checked against the loop

The VIS kernels accumulate products in 16-bit lanes and widen to 32-bit
only when the lane would overflow. Each kernel carries a comment naming
its budget. Two of them were being enforced far more often than the
loop actually required.

**Q5_K folded four times as often as needed.** The comment reads "a
16-bit lane holds only 8 terms (8 * 31 * 127 = 31496)", which is
correct. But the loop is `for (l = 0; l < 32; l += 8)` -- four
iterations, each adding *one* term per lane. The worst case is
`4 * 31 * 127 = 15748`, less than half the budget. The mid-loop fold
was guarding an overflow that could not occur, at a cost of 12 extra
`hsum16` per sub-block:

```
Q5_K   391 -> 347 instructions, 146 -> 129 memory ops   (-11%)
```

This is almost certainly the long-standing anomaly recorded in the
SPARC profile -- Q5_K costing 2.55x the microseconds per call of Q4_K
despite being only 25% wider. The extra width never explained a 2.55x
gap; four times the fold traffic does.

**Q6_K widened every iteration when it could hold four terms.** Two
iterations, two terms each, `4 * 63 * 127 = 32004` against a 32767
limit. It fits, with 2.3% to spare:

```
Q6_K   1395 -> 1236 aligned, 1614 -> 1441 unaligned      (-11%)
```

That margin is too thin to leave as arithmetic on paper.
`tests/t_vissat.c` now drives every lane to its worst case -- all
weights at maximum, all activations at the +-127 clamp -- and requires
bit-identity with `i8`. Both formats pass.

**Folding the accumulator chains costs nothing.** Q4_K and Q5_K each
run two independent chains to hide the 3-cycle VIS latency, then called
`hsum16` on both. Adding them with a single `fpadd16` first and taking
one `hsum16` keeps the chains independent through the loop -- the merge
happens once, after it:

```
Q4_K   261 -> 253      Q5_K   347 -> 343
```

**The lesson:** a lane budget is a property of the *loop*, not of the
format. Both comments described the format's limit correctly and both
kernels then enforced it against the wrong iteration count. Worth
re-deriving the term count from the loop bounds whenever one of these
is written, and worth a test that actually saturates the lanes rather
than trusting the arithmetic.

## 30. us/call is the wrong unit when call sizes differ 6x

The SPARC profile groups matvec time by weight format. Read as
microseconds per call, it says Q6_K is the most expensive format on the
machine -- 5.16x the cost of a Q4_K call. That reading drove the whole
1.15.0 effort, and it is wrong.

The formats do not carry equal work per call:

```
Q4_K   299,892,736 elements / 2646 calls =   113,338 elements/call
Q5_K   150,994,944 elements /  972 calls =   155,345 elements/call
Q6_K   300,417,024 elements /  443 calls =   678,142 elements/call
```

A Q6_K call carries **6x** the weights of a Q4_K call, because Q6_K
holds `token_embd`, the LM head and the FFN down-projections -- the
largest tensors in the model. Normalised per weight, on the same runs:

```
nanoseconds per weight       Q4_K     Q5_K     Q6_K
i8   1.11.0 (32-bit)       618.05   975.41   508.35
i8   1.12.1 (64-bit)       590.05  1097.44   435.96
vis  1.14.2 (best of 3)    495.26  1151.72   426.90
```

**Q6_K has always been the cheapest format per weight on this machine**,
including when it ran the portable integer path with no VIS kernel at
all. It occupies ~27% of matvec time because it is ~40% of all weights,
not because it is slow. Its 210-byte super-block carries 256 weights at
6.56 bits each, so it streams fewer bytes per MAC than Q4_K's effective
4.5 -- and on a machine this memory-bound, bytes per MAC is what a
kernel is really paying.

The format that stood out was **Q5_K, at 2.3x the per-weight cost of
Q4_K** despite being only 25% wider. That was the real anomaly, and it
was a code defect (finding 29), not a property of the format.

### What the hardware said about the Q6_K kernel

The kernel written on the strength of the misread profile is correct,
bit-identical to `i8`, and cuts the instruction count 3.1x (4180 ->
1338 per super-block). On an UltraSPARC IIi it bought nothing:

```
Q6_K / Q4_K cost per weight, same run (cancels machine state)
  1.14.2, Q6_K on portable i8      0.862
  1.15.1, Q6_K on the VIS kernel   0.855
```

0.8%. The 1.15.1 run was taken in a different session and is inflated
overall -- `rms norms`, which no version touched, is 4.3x slower in it
-- so absolute times across the two are not comparable. Correcting by
any factor consistent with the untouched stages (1.14x to 1.28x) puts
the Q6_K change between **+8% and -4%**. Under every plausible
correction, the 3.1x instruction reduction is invisible.

Q6_K was never instruction-bound. The kernel stays in the tree behind
`-DINFER_VIS_Q6K` and is **off by default**, because the default
should be the path that measured faster. There is no `make` target for
it — the ISA ladder of per-variant targets was collapsed in 1.21.0
(finding 50), so pass the define by hand:

```sh
gmake solaris SUNOPT='-xO5 -xunroll=16 -DINFER_VIS_Q6K'
```

> **RETRACTED (1.25.0) — this subsection only.** The verdict above is
> wrong. The VIS Q6_K kernel is **on by default** since 1.25.0 and is
> worth **−29% on `matvec Q6_K`** and **13.87 → 9.48 s/tok** on the
> real UltraSPARC IIi. What failed here was the evidence, not the
> reasoning: the 1.15.1 session was contaminated, so the 0.8% ratio it
> produced was never a measurement of the kernel. "Inconclusive" was
> recorded as "no gain". See **finding 61**.
>
> The rest of finding 30 — that `us/call` is the wrong unit, and that
> Q6_K's profile share reflects its weight count — still stands, and is
> still the reason the 1.15.0 effort was misdirected.

### The rules this produces

- **Normalise by work, not by call.** A profile grouped by anything
  whose call sizes differ must be divided by elements before formats
  are compared. `us/call` compares tensor shapes, not kernel quality.
- **Ratios within one run are the only safe cross-version comparison**
  when runs come from different sessions. Absolute seconds carry the
  machine's mood; ratios cancel it.
- **Always check a stage nobody touched.** `rms norms` moving 4.3x is
  what proved the 1.15.1 log was contaminated. Without that control the
  Q4_K "regression" in the same log looks real -- it is 3 instructions
  shorter and measured 8.5% slower.

## 31. The same idea, opposite verdicts on two backends

`i8_dot_q4_K` walked both nibble streams in one interleaved loop:

```c
for (l = 0; l < 32; l++) {
    int w = qs[l];
    acc1 += (w & 0xF) * x1[l];
    acc2 += (w >> 4)  * x2[l];
}
```

Splitting it into two separate reductions -- one per stream -- is worth
**1.25x on x86-64** and **1.08x on the i486/Geode build**, bit-identical
either way. It is pure deletion of interleaving; no new arithmetic.

The interesting part is that **"split the loops" was already measured
and rejected for the MMX backend at 0.90x** (see the rejected-optimisation
list). Same transformation, same format, opposite sign:

- In hand-written MMX the two streams share loaded registers. Splitting
  forces the packed bytes to be re-loaded and re-masked for the second
  pass, so it costs a full extra load+mask per 8 weights.
- In portable C the compiler was juggling two dependency chains and two
  activation pointers through one loop body. Separated, each loop is a
  textbook reduction it can unroll and software-pipeline freely, and
  `qs[l]` stays in a register across both uses of the same cache line.

**A transformation is not good or bad in itself; it is good or bad
against a particular register allocator.** Anything on the rejected list
for one backend is worth re-testing on another rather than assumed dead.

### Two things that did NOT work

**Byte-domain nibble masking, ported from MMX: 0.78 -> 1.03 ns/weight.**
The MMX kernel wins by masking four nibbles with one `pand` before
widening (that was the 1.5.0 change). Hand-assembling the same 32-bit
word in C from four `qs[]` bytes costs more than the byte loads it
replaces, because the compiler already keeps those bytes in a register
and folds the mask into the multiply. Reverted.

**Splitting Q5_K the same way: 1.43 -> 1.51 ns/weight.** Q5_K reads two
planes, `qs[l]` and `qh[l]`. Splitting doubles the loads of both, and
the hi-bit shift has to be redone per stream. Q4_K reads one plane, so
it does not pay that. Reverted; only Q4_K is split.

### Measuring this needed care

Comparing the two kernels through their float results reported
differences on `-mfpmath=387` builds and none on x86-64 or SPARC. The
integer accumulators are identical on all three -- verified over 200,000
random sub-rows per architecture in `tests/t_i8split.c`. What differed
was x87 80-bit intermediates being spilled at different points because
the reference lived in another translation unit; `-ffloat-store` or
`-mfpmath=sse` makes it vanish.

**Never diff a float result to validate an integer change.** Compare the
integers.

### And the trend that motivated it holds everywhere

Cost per weight relative to Q4_K in the same run, best kernel per
platform:

```
                     Q5_K    Q6_K
Geode  mmx 1.6.0     1.24    0.68
i7     mmx 1.6.0     1.24    0.80
SPARC  vis 1.15.1    1.15    0.86
```

Q6_K is the cheapest format per weight on **every** platform, and Q4_K
is the one carrying 40% of the weights. The SPARC-only conclusion from
finding 30 generalises. Notably the Q5_K outlier was fixed independently
on x86 back in 1.5.0 (byte-domain masking, 52 -> 26 instructions per 16
weights) and on SPARC in 1.15.1 (removing a redundant fold) -- the same
class of defect, found ten versions apart.

## 32. A lookup table that stopped paying, and then blocked vectorisation

`i8_dot_q5_K` read its nibbles through a pair of 256-byte tables:

```c
static unsigned char nib_lo[256];   /* i & 0x0F */
static unsigned char nib_hi[256];   /* i >> 4   */
```

The rationale, in a comment above them, was that two L1-resident loads
beat an AND and a SHIFT on a 486. That comment also claimed "~2.4x on
the Q4_K inner loop" -- but **Q4_K never used the tables**, in this
version or any earlier one. Only Q5_K did.

Two separate problems.

**They block vectorisation.** A table lookup is a gather. No
autovectoriser can do it, so under `-march=haswell` Q5_K stayed scalar
while Q4_K and Q6_K sped up 2.2x and 1.7x around it. Q5_K is 20% of the
model's weights and grew to **56% of matvec time**, cancelling the
entire SIMD win: the whole-model figure was 3.18 tok/s with the tables
and 4.58 without.

**They were not faster scalar either.** Measured with the tables
removed, on the two targets that ship:

```
i8_dot_q5_K, static instruction count
  sparc64  155 -> 147   (35 -> 30 loads)
  i486     168 -> 165

Q5_K, i486-targeted build, ns/weight, best of 7
  table       2.224
  arithmetic  1.984      1.12x
```

An AND and a SHIFT are one cycle each and pipeline freely; a load has
latency even from L1, and two loads per weight is two more slots in a
loop that is already load-bound. Whatever advantage the tables had on
a real 486 did not survive to any machine this project currently
targets.

Removed unconditionally -- no `-march` gate, because nothing measured
faster with them. `nib_init()` and both tables are gone.

**The lesson is about comment rot as much as caching.** The
justification named a kernel that did not use the code, and no test
covered the claim, so it survived unexamined through fifteen releases.
An optimisation whose rationale cannot be re-run is a liability.

## 33. A static backend priority list is wrong once the compiler vectorises

`bk_select("auto")` walks a fixed table and takes the first entry the
CPU supports:

```c
{ "vis", ... }, { "mmx", ... }, { "i8", ... }, { "ref", ... }
```

The ordering encodes "a hand-written SIMD kernel always beats portable
C". That held for fifteen releases and stopped holding the moment
`NATIVE=1` existed. Measured on the same machine, same model, same
seed:

```
make linux NATIVE=1
  --backend mmx    2.456 tok/s     <- what auto picked
  --backend i8     3.274 tok/s     <- 1.33x faster
```

The MMX kernel is 64-bit SIMD written by hand. Under `-march=native`
the compiler vectorises the *portable* `i8` path to 128/256-bit SSE2 or
AVX2, and wins comfortably. Auto-selection handed the user a 33%
slowdown and reported it as the fast path.

The fix is a compile-time test, not a runtime benchmark:

```c
#if defined(__AVX2__) || defined(__SSE2__)
#define I8_IS_VECTORISED 1
#endif
```

`__SSE2__` and `__AVX2__` are defined by the compiler exactly when it
may emit those instructions, which is exactly when `i8` gets
vectorised. If set, `auto` prefers `i8` over the hand-written kernels.
The default i486 build defines neither, so its selection is unchanged --
verified by diffing the disassembly of `i8_dot_q4_K` before and after:
byte-identical, and the object file is 64 bytes *smaller* because the
dead branch folds away.

**The general point:** a priority list is a cached benchmark result. It
is only valid for the build configuration it was written against, and
adding a build flag can silently invalidate it. Anything that ranks
implementations should either re-derive the ranking from the build
configuration, as here, or measure.

## 34. The i486 baseline is real; the binary still needs a Pentium II

Every claim in this project about running on a 486 has been about *our*
code. That part holds — compiled with the shipping flags, every
translation unit contains **zero** CMOV and zero SSE/AVX instructions.

The linked binary is another matter. Under `qemu-i386 -cpu 486` the
default build dies before reaching `main()`:

```
qemu: uncaught target signal 4 (Illegal instruction)

IN: _dl_aux_init
0x0805eb79:  0f 42 c2   cmovbl %edx, %eax
```

`_dl_aux_init` is glibc's own startup. A static `printf("hi")` built
with `-march=i486` fails identically, so this is not our code at all.
Emulated floor for the shipped binary:

```
486        Illegal instruction
pentium    Illegal instruction
pentium2   mmx  <- selected      <-- first CPU that works
pentium3   mmx  <- selected
n270 / core2duo / Haswell   all fine
```

Modern glibc targets i686. The i486 baseline still buys the Geode,
which is a 486-class core *with* MMX and runs everything from
`pentium2` upward, and it keeps `ref` genuinely portable for anyone
linking a different libc.

**This is the same shape as the mingw CRT finding**: the Windows builds
are Pentium-Pro-and-later because `strtod`/`pow`/`ldexp` in the CRT use
CMOV, not because our kernels do. Two runtimes, same conclusion, and in
both cases the compiler flags were doing exactly what they claimed.

A real 486 needs a libc compiled for one — musl or uClibc-ng, static.
Debian's `musl-gcc` is x86-64 only, so that route is **documented but
untested here**.

**The rule:** keep asserting our objects are clean, because that is
what we control and CI can check it. Do not extend the claim to the
linked binary without running it on the target. "Built to an i486
baseline" and "boots a 486" are different statements.

## 35. Which loop shape is fastest depends on the compiler flags

`i8_dot_q5_K` reads two planes, `qs` and `qh`, and produces two
accumulators. Two ways to write it:

- **interleaved** -- one loop, each plane read once, two chains
- **split** -- two loops, each a clean reduction, both planes re-read

1.15.3 measured the split and rejected it: it doubles the loads, and
scalar that costs more than it saves (i486 165 -> 170 instructions,
sparc64 147 -> 160).

That verdict inverts the moment SSE2 is available. Interleaved, the
vectoriser produces something poor; split, each loop is a textbook
reduction. On a Dothan-targeted build (`-march=pentium-m -msse2
-ftree-vectorize`), whole-model:

```
matvec Q5_K   3.489 s -> 1.853 s     1.88x
end to end    2.756   -> 3.308 tok/s 1.20x
```

Without the split, SSE2 was barely worth having: Q4_K went 1.87x
faster and Q6_K 1.28x, but Q5_K went **0.56x** -- backwards -- and
swallowed the rest, leaving 1.07x on matvec overall. It is 20% of the
model's weights and had become 48% of matvec time.

So the shape is selected by the same predicate as finding 33:

```c
#if defined(__SSE2__) || defined(__AVX2__)
    /* split: two clean reductions */
#else
    /* interleaved: one pass, fewer loads */
#endif
```

The i486 object is **byte-identical** to before the change, verified by
diffing disassembly. Both shapes are the same reassociation of
independent integer sums -- proven exact over 200,000 random sub-rows
on x86-64, i486, SSE2 and big-endian sparc64 (`tests/t_q5split.c`).

**The pattern, now three times over:** finding 31 (split helps i8, hurts
MMX), finding 33 (priority list wrong once i8 vectorises), and this. An
optimisation is not fast or slow in itself; it is fast or slow against
a specific code generator. A rejected result is only valid for the
configuration it was measured in, and adding a build flag can flip it.

## 36. Instruction count is not the i486's cost model either

The 1.15.3 Q4_K split was justified on x86-64 and a modern host. Its
effect on the i486-targeted build looked like a regression:

```
i8_dot_q4_K   151 -> 159 static instructions
```

Eight more instructions, on the one target where instruction count is
supposed to matter most. Measured, on the same build:

```
Q4_K   1.788 -> 1.609 ns/weight     1.11x FASTER
Q5_K   2.219 -> 2.040 ns/weight     1.09x FASTER
```

End to end with `--backend i8`: 1.277 -> 1.386 tok/s, 8.5% faster.

The split adds loop overhead but removes the interleaving of two
dependency chains and two activation pointers through one body. Even a
486 has a pipeline; two clean reductions schedule better than one
tangled loop, and that outweighs eight extra instructions.

**The i486 default is untouched regardless.** It selects `mmx`, and
`backend_mmx.c` compiles to a byte-identical object -- none of this
session's work touched the hand-written kernels. Default build,
end to end: 2.435 -> 2.427 tok/s, i.e. noise.

**So there was nothing to gate.** The instinct to gate any regression
is right; the instinct to trust a static instruction count as evidence
of one is not. This is the fourth time in this project that counts
pointed the wrong way (see findings 28, 30, 31), and the second time
they did so on the 486 specifically. Measure the target, then decide
whether a gate is needed.

## 37. Two logs, six releases apart, read as a backend comparison

The uploads directory holds `perf-i7-9th-i8.txt` and
`perf-i7-9th-mmx-1.6.0.txt`. Nothing in the filenames says so, but the
first is **infer 1.0.0** and the second is **1.6.0**. Compared as if
they were a backend A/B they give i8 = 0.44x of mmx, and that number
was repeated in this project's own analysis.

It is wrong. Same version, same host:

```
1.0.0 Geode   i8 0.026   mmx 0.028   0.93x
1.0.0 i7      i8 1.236   mmx 1.265   0.98x
1.6.0 Geode   i8 0.030   mmx 0.063   0.48x
```

**At 1.0.0 the backends were within 2-7% of each other.** The gap is
not architectural; it is 1.3.0-1.5.0 of MMX kernel work (2.08x on the
Geode) that i8 did not receive. The 0.44x figure measures six releases
of development and attributes it to instruction sets.

`ref`, meanwhile, has always been far behind: ~0.45 tok/s against i8's
1.27 in the same run, and 6.4x behind i8 on Q6_K specifically.

**Two rules this produces:**

- **Version-stamp every log, and check it before comparing.** These
  files carry the version in their first line; the filenames do not,
  and the filenames are what got read.
- **A backend comparison must come from one binary.** Different builds
  differ in more than the flag you are studying. The same mistake in a
  different costume produced the 1.15.1 "regression" (finding 30) --
  two runs, fifteen hours apart, treated as comparable.

## 38. A per-format benchmark, and the limits of a synthetic one

`--kernel bench` times every available kernel on every weight format
using synthetic super-blocks, then pins the winner per format. It needs
no model, scales its repeat count to the host (~40 ms per measurement,
so a 440 MHz Ultra and a modern x86 spend similar wall time), and takes
a second or two.

The dispatch cost is nothing. One table lookup per matrix row-block --
about 113,000 weights of work -- measured against a build with the
table compiled out, interleaved, six samples each:

```
  table=2.210  bypass=2.384
  table=2.407  bypass=2.393
  table=2.373  bypass=2.410
  table=2.450  bypass=2.415
  table=2.382  bypass=2.248
  table=2.423  bypass=2.350
```

Fully overlapping; the table wins three of six.

**The interesting part is where it disagrees with reality.** On the
64-bit build the benchmark produced a genuinely mixed assignment that
no single `--backend` could express:

```
  q4k   i8      q5k   mmx      q6k   i8      q8_0  i8
```

End to end, that mix measured **3.316 tok/s against 3.334** for plain
`auto`. Slightly *worse*, not better.

The synthetic benchmark runs one format at a time, on freshly written
buffers, with the activation vector hot. Real inference interleaves
formats across 24 layers, streams ~344 MB of weights per forward pass,
and evicts constantly. A kernel that wins in isolation can lose in
that traffic -- the same reason the isolated i8 nibble LUT measured
2.46x and delivered 0.97x (finding 28), and the reason instruction
counts have pointed the wrong way four times in this project.

So `bench` is **a measurement tool, not an optimiser**. It answers "which
kernel is faster on this machine for this format, in isolation", which
is exactly the question that could not be answered before without
rebuilding. It does not promise that the winning combination is faster
end to end, and on the one host tested it was not. Pin what it finds
only after confirming with `--log-perf` on the real model.

That is also why it stays opt-in rather than running at startup: an
automatic version would have silently made this machine slower.

## 39. `universe` is not enabled on GitHub's runners

The v1.16.1 tag failed CI in 15 seconds, at step 3, exit code 100 —
before a single line was compiled. The step was the toolchain install,
and the cause was one package added for the new SPARC checks:

```
sudo apt-get install -y ... gcc-sparc64-linux-gnu ...
```

`gcc-sparc64-linux-gnu` exists in Ubuntu 24.04, but in **universe**,
which `ubuntu-latest` does not enable by default. `apt-get` exits 100
for the whole command, so every package in the list fails together and
the build never starts.

Two things worth noting:

- **`qemu-user-static` and `gcc-mingw-w64-i686-win32` are also in
  universe.** They had been installing fine only because the runner
  image happens to pre-install mingw and its dependencies. That is an
  accident of the image, not a guarantee, and the same failure was one
  image refresh away.
- The pre-existing s390x step installs its toolchain **inline, where it
  is used**, which is why it never hit this. New cross-toolchains
  should follow that pattern.

Fixed by enabling the repository once, up front:

```yaml
sudo add-apt-repository -y universe
sudo apt-get update
```

and by making the SPARC toolchain install non-fatal:

```yaml
- name: install the SPARC cross-compiler
  id: sparc
  run: |
    if sudo apt-get install -y gcc-sparc64-linux-gnu; then
      echo "have=yes" >> "$GITHUB_OUTPUT"
    else
      echo "::warning::gcc-sparc64-linux-gnu unavailable; skipping SPARC checks"
      echo "have=no" >> "$GITHUB_OUTPUT"
    fi
```

with the three SPARC steps gated on `steps.sparc.outputs.have == 'yes'`.

**The rule:** a cross-target check is a bonus, not a gate. If its
toolchain disappears from the archive, the x86 release must still
build. An optional check that can fail the whole pipeline is worse than
no check, because it fails at the moment you most want a release.

The tag itself was fine — rebuilding v1.16.1 locally with `universe`
available produces all six artifacts and passes every assertion.

## 40. Two CI faults: a Recommends, and a second workflow

### The SPARC step failed because a dependency is only *recommended*

`gcc-sparc64-linux-gnu` does not depend on `libc6-dev-sparc64-cross`;
it only **Recommends** it. Without that package there is no `libc.a`,
so `-static` quietly produces a dynamic binary and qemu then fails at
load time, not at compile time:

```
qemu-sparc64-static: Could not open '/lib64/ld-linux.so.2'
```

The step compiled fine and died running the first test, which made it
look like a kernel bug rather than a missing package.

Fixed by installing the cross libc explicitly and, more usefully, by
**proving the toolchain works before trusting it**: build and run a
static `int main(void){return 0;}` and only then set `have=yes`. A
capability check beats an installation check, because it fails for the
right reason and in one place.

The test loop also swallowed which test failed. `for t in ...; do
qemu $t; done` returns the status of the last iteration only; it now
reports the failing test by name and exits.

### The release was published by a workflow nobody was looking at

`build.yml` gates its release job correctly:

```yaml
release:
  needs: build
  if: github.ref_type == 'tag'
```

`needs:` alone is sufficient — a failed `build` skips `release`, and
the v1.17.0 run confirms it: build failed, release ran 0s, no release
was created.

But at v1.15.1 the repository still contained a **second** workflow,
`release.yml`, left over from before the two were merged:

```yaml
on:
  push:
    tags: ['v*']
jobs:
  release:          # needs: (none)   if: (none)
```

Same trigger, no dependency on anything. It rebuilt from scratch and
published regardless of whether `build.yml` had passed — which is why
a release appeared for a tag whose checks were red, and why the
published binaries were not the ones CI verified.

It was deleted in 1.16.0, so this is already fixed on `main`; the
symptom was visible on older tags.

**The rule:** one publisher. Two workflows on the same trigger is not
redundancy, it is a race in which the careless one usually wins.

### Postscript: the emulated test then failed for a third reason

With both faults fixed, run #26 still failed — `t_vis`, under the
runner's qemu 8.2.2, where it passes locally on 10.0.11. QEMU's
`fmul8x16` helper does differ between those releases (`if ((tmp & 0xff)
> 0x7f) tmp += 0x100` against a plain `+ 0x80`), but both rules give
identical results for all 16320 (weight, activation) pairs the test
checks, so the rounding change is not it. Debian trixie carries no
qemu 8 to reproduce with.

Rather than guess a third time, the emulated step is now
`continue-on-error` and prints both tool versions. The SPARC
cross-**build** still gates, because compilation failures are real
failures. Emulated *execution* is advisory: it has value as a signal
and none as a gate, for the same reason finding 30 gives — the emulator
models semantics, not the machine.

## 41. Installing a cross-toolchain can break a target that already built

Four CI runs in a row failed on this workflow, each for a different
reason, and the last one was the instructive one:

```
== solaris ==            (built fine)
== solaris NO_VIS=1 ==   (built fine)
== solaris PROFILE=1 ==  (built fine)
cc ... -m32 ... src/net_posix.c
/usr/include/linux/errno.h:1:10: fatal error: asm/errno.h: No such file
```

The 32-bit x86 build had succeeded minutes earlier in the same job. It
broke because `gcc-sparc64-linux-gnu` and `libc6-dev-sparc64-cross`
were installed *after* it, and that pulled a `linux-libc-dev` change
which removed the i386 headers `gcc-multilib` depends on. `apt` did
nothing wrong: asked to satisfy the SPARC packages alone, it had no
reason to preserve an arch nobody in that transaction needed.

**Three structural mistakes, not three bugs:**

1. **Toolchains were installed in several steps.** apt can only
   reconcile requirements it sees together. Everything now goes in one
   transaction, with `libc6-dev-i386` named explicitly so the i386
   headers are a stated requirement rather than an accident of
   `gcc-multilib`'s dependency graph.
2. **Availability was assumed, not tested.** A single `probe` step now
   compiles and runs a trivial program for each target and records
   `yes`/`no`. Later steps are gated on that. A capability check fails
   once, in one place, with one message -- instead of surfacing three
   steps later as a missing header.
3. **Steps destroyed artifacts other steps needed.** `make clean` in
   the flag matrix and the SPARC steps removed the x86 binaries that
   every later step refers to by name, giving exit 127 on
   `--kernel selection`. Each destructive step now ends with
   `make all`.

**The rule:** in CI, an environment change is a global side effect. Do
them all at once, at the start, then verify what you actually have
before depending on it. Anything optional -- a cross-compiler, an
emulator -- must be probed and skipped, never assumed and never allowed
to fail the pipeline for a platform it has nothing to do with.

## 42. `apt-get install` is atomic, and `|| true` on it hides everything

Finding 41 said "do all the environment changes at once, at the start".
Correct, and 1.17.3 implemented it as a single `apt-get install` with
nine package names and a trailing
`|| echo "::warning::some cross-toolchains unavailable"`.

Run #32 failed at *"confirm the win32 (not posix) mingw variant"* with
**exit 127** — `i686-w64-mingw32-gcc: not found`. mingw was not the
problem. **Nothing had been installed**, `cc -m32` included.

Two independent mistakes had to line up:

**`apt-get install` is all-or-nothing.** If one name on the line cannot
be resolved, apt exits 100 having installed *none* of the others.
Reproduced directly:

```
$ apt-get install -y --dry-run coreutils definitely-not-a-real-package-xyz
E: Unable to locate package definitely-not-a-real-package-xyz
rc=100          # coreutils not installed either
```

So one unavailable *optional* cross-compiler silently took out
`gcc-multilib`, `libc6-dev-i386` and mingw. The `||` was written for
"a cross target is missing"; it actually caught "the entire toolchain
is missing" and printed a warning.

**The probe was advisory when it should have been decisive.** The very
next step *did* detect the truth and emitted four warnings —
`x86_32 unavailable`, `win32 unavailable`, `sparc`, `s390x` — and then
let the run continue. Everything downstream assumed a compiler existed.
The failure surfaced four steps later, pointing at the wrong component.

Fixed by splitting on *criticality*, not by adding package names:

| group | packages | failure mode |
|---|---|---|
| required | build-essential, gcc-multilib, libc6-dev-i386, mingw ×2, qemu-user-static | own transaction, **no `\|\|`** — fails the step |
| optional | sparc64 gcc + libc, s390x gcc | **one `apt-get` per package** — warns, skips that target only |

and by making the probe **fail the run** if `x86_32` or `win32` cannot
compile a hello world, printing the compiler's own stderr and naming
the package that did not install. `sparc`/`s390x` stay advisory.

**The rule:** `cmd_that_installs_many_things || echo warning` is never
right. Either the thing is required — then let it fail, loudly, at the
step that caused it — or it is optional, and it must be attempted
*individually* so its absence cannot take down anything else. A green
step that installed nothing is worse than a red one.

## 43. A probe must be at least as hard as the build it gates

Finding 42's fix worked — toolchains installed, steps 1–9 green for the
first time in five runs. Run #34 then died at step 10 (`make all`,
exit 2), one step after the probe reported `x86_32: yes`.

The probe compiled this:

```c
int main(void){return 0;}
```

which needs **no kernel headers at all**. Meanwhile `src/net_posix.c`
includes `<errno.h>`, which reaches `<asm/errno.h>`. Demonstrated by
hiding the i386 kernel headers and running both:

```
$ cc -m32 trivial.c        -> links fine        ("x86_32: yes")
$ cc -m32 includes_errno.c -> fatal error: asm/errno.h: No such file
```

So the probe certified a toolchain that could not build the project.
Every guarantee downstream was gated on a test weaker than the thing it
was protecting.

**And the cause of the missing headers was finding 41, returning,
because Ubuntu is not Debian.**

| | Debian | Ubuntu |
|---|---|---|
| `linux-libc-dev` | `Architecture: all` | arch-qualified: `amd64`, `i386` |

`libc6-dev-sparc64-cross` → depends on `linux-libc-dev-sparc64-cross`.
Resolving the cross libcs can replace the shared `linux-libc-dev` and
take the i386 headers with it. apt is not misbehaving: no package in
that transaction declared it needed i386. 1.17.4 installed the optional
cross toolchains *after* the required set — reopening the hole 1.17.3
had closed. This is why the bug is Ubuntu-only and never reproduced on
the Debian sandbox.

Two fixes:

1. The probe compiles the headers `net_posix.c` really uses
   (`errno.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`); mingw
   gets `winsock2.h`/`ws2tcpip.h`. Verified in **both** states — passes
   healthy, and with the i386 headers removed it fails *at the probe*,
   printing the compiler's own error and naming `linux-libc-dev:i386`.
2. `linux-libc-dev:i386` is named explicitly and re-asserted **after**
   the cross toolchains, making the 32-bit headers a stated property of
   the final state rather than a survivor of dependency resolution.

**The rule:** a capability probe must exercise the same headers,
flags and libraries as the build it authorises. If the probe is easier
than the build, it converts a clear failure at the right step into a
confusing one several steps later. And when guarding against a package
manager, assert the end state *after* every package operation — not
once at the beginning.

## 44. Raising the ISA baseline changed the output, and FMA was why

Adding an SSE2/AVX2 ladder for Windows (1.18.0) produced a clean
speed ladder on one Xeon, 20 tokens, greedy, best of 3:

| baseline | tok/s | vs 32-bit | `auto` picks |
|---|---|---|---|
| i486 + x87 | 1.61 | 1.00× | `mmx` |
| Pentium 4 / SSE2 | 2.01 | 1.25× | `i8` |
| x86-64 | 2.27 | 1.41× | `i8` |
| Haswell / AVX2 | 2.90 | **1.80×** | `i8` |

No new kernel was written. This is finding 33 paying out: once the
compiler may emit SSE2 it vectorises the portable `i8` path, and `auto`
correctly stops preferring the hand-written MMX kernel.

**But the AVX2 build generated different text.** Greedy, fixed seed,
diverging at about token 8:

```
...Paris. It serves as the nation's political, cultural, and economic center.
...Paris. It serves as the nation's largest city and the seat of its government,
```

`tests/t_ident` still reported every integer kernel bit-identical, so
it was not the kernels. The cause is float contraction: `-march=haswell`
lets GCC fuse `a*b+c` into one `vfmadd` — 27 of them in `quant.c` alone
— which keeps the product at full internal width instead of rounding it
to `float` first. Strictly *more* accurate, and different from every
other build.

Confirmed by construction, in both directions:

- rebuilding the AVX2 target with `-ffp-contract=off` reproduces the
  SSE2/64-bit output **exactly** (same md5);
- rebuilding the 32-bit target with `-mfpmath=sse` reproduces it too —
  proving the remaining 32-bit difference is x87's 80-bit intermediates
  and not a bug.

Cost of pinning it: **2.83 → 2.75 tok/s, 2.8%**. Taken. A user who
downloads a faster binary and gets different answers will reasonably
conclude the fast one is broken, and "both roundings are valid" is not
a satisfying reply. CI now greps the Makefile for the flag *and* counts
`vfmadd` in the shipped exe.

The 32-bit x87 build keeps its own answer, unavoidably — you cannot
have an i486 baseline and SSE rounding at the same time.

**The rule:** changing the instruction baseline is a numerical change,
not just a speed change. Any build matrix that spans x87, SSE and FMA
needs an output-equivalence check across tiers, or the fastest binary
quietly becomes the odd one out.

### Aside: what actually stops the 32-bit binaries on a 486

Re-verified while auditing the ladder, because it is easy to
misremember. Our objects are genuinely 486-clean — 0 CMOV, 0 xmm/ymm,
under both `gcc -m32` and mingw. The *runtime* is not:

- **Linux:** `ld.so` faults before `main`. Traced under
  `qemu-i386 -cpu 486`: `cmovnel %edi,%ecx` inside the loader.
- **Windows:** the mingw CRT contains 36 CMOV, but all of them are in
  `strtod` / `pow` / `ldexp` / `*_D2A` helpers — none on the startup
  path.

So the floor is a Pentium II for reasons outside our code, exactly as
finding 34 said. Nothing regressed; the MMX build is byte-identical.

## 45. A probe with a hardcoded list is a probe with a blind spot

Finding 43 replaced a trivial probe with one that compiles the real
headers, and said: *a capability probe must exercise the same headers,
flags and libraries as the build it authorises.* Run #36 found the hole
that sentence left open — the probe checked the right **things**, but
against a **hardcoded list of tools**.

1.18.0 added 64-bit Windows builds, which invoke a second mingw
(`x86_64-w64-mingw32-gcc`). The workflow named only the 32-bit one, in
two places: the apt transaction and the probe. So the compiler was
never installed, the probe never looked for it, nine steps went green,
and `make all` died:

```
make[1]: x86_64-w64-mingw32-gcc: No such file or directory
make[1]: *** [Makefile:389: windows] Error 127
```

The tempting fix is to add the package name to both lists. That fixes
this instance and guarantees a repeat the next time a target gains a
toolchain — the failure mode is *duplication between the Makefile and
the workflow*, not a missing string.

Fixed by removing the duplication:

1. **`make printvar VAR=X`** prints any Makefile variable, so CI can
   ask which compilers the build intends to use:
   ```
   CC32=$(make -s printvar VAR=CC)
   WCC32=$(make -s printvar VAR=WINCC)
   WCC64=$(make -s printvar VAR=WINCC64)
   ```
2. **A completeness assertion.** `make -n all` is parsed for every
   compiler it would invoke, and each must exist. If a future target
   adds a toolchain the probe doesn't know about, *that* step fails
   with a message naming the tool — the probe can no longer be
   silently incomplete.

Verified in both directions: healthy runner reports
`x86_32/win32/win64: yes`; with the 64-bit mingw hidden, the probe
fails at step 5 naming `gcc-mingw-w64-x86-64-win32`, instead of at
step 10 with `Error 127`.

**The rule:** any list in CI that restates something the build system
already knows will drift, and it will drift silently. Derive it, or
assert the two agree. "Remember to update both" is not a mechanism.

## 46. The ISA ladder on real Windows: 1.37×, not the predicted 1.80×

First execution of the Windows binaries on real hardware (i7-9750H,
Windows, 19-token prompt + 10 generated, 29 forward passes, all six
logs from one session minutes apart — an unusually clean comparison).

| build | bits | backend | gen tok/s | prompt tok/s | matvec Mflop/s |
|---|---|---|---|---|---|
| `windows.exe` | 32 | mmx | 2.152 | 3.318 | 3691 |
| `windows-sse2.exe` | 32 | i8 | 2.421 | 3.801 | 4263 |
| `windows64.exe` | 64 | i8 | 2.550 | 4.298 | 4863 |
| `windows64-avx2.exe` | 64 | i8 | **2.942** | **4.831** | **5684** |

The ladder is monotonic and every tier is a real gain — the shipped
matrix is justified. But the honest headline is **1.37×**, against the
**1.80×** I extrapolated from the sandbox Xeon:

| tier | Xeon predicted | i7 measured |
|---|---|---|
| 32 sse2 | 1.25× | 1.12× |
| 64 base | 1.41× | 1.18× |
| 64 avx2 | **1.80×** | **1.37×** |

Prompt processing, which is more compute-bound and carries less
sampling overhead, reaches 1.46×. Generation is the weaker number
because per-token work outside matvec does not shrink.

**Why the sandbox over-predicted:** the Xeon's 32-bit baseline was
unusually slow (1.61 tok/s vs the i7's 2.152), so every ratio measured
against it was inflated. Ratios taken against a weak baseline on a
shared virtual machine do not transfer to real silicon. Absolute
numbers from a cloud sandbox should be treated as a smoke test, not a
prediction.

### Per-format: the gain is almost entirely Q4_K

ns per weight, derived from us/call ÷ elements-per-call (finding 30):

| build | Q4_K | Q5_K | Q6_K |
|---|---|---|---|
| 32 mmx | 11.91 | 14.99 | 9.46 |
| 32 sse2 | 8.53 | 15.67 | 8.64 |
| 64 base | 7.17 | 14.00 | 7.74 |
| 64 avx2 | 6.91 | 10.87 | 6.42 |

Speedup versus the 32-bit MMX kernel:

| build | Q4_K | Q5_K | Q6_K |
|---|---|---|---|
| 32 sse2 | 1.40× | **0.96×** | 1.10× |
| 64 base | 1.66× | 1.07× | 1.22× |
| 64 avx2 | 1.72× | 1.38× | 1.47× |

**Q5_K gets slower on the 32-bit SSE2 build** — 0.96×, the only
regression in the matrix. Finding 35 gated the Q5_K split-loop shape on
`__SSE2__` because the split won 1.88× *in a microbenchmark under
SSE2*. On this real CPU, in the whole model, that choice is a small
net loss at the 32-bit SSE2 tier and only pays off once AVX2 is
available. The relative-cost table shows the same thing: Q5_K costs
1.26× a Q4_K weight under MMX but 1.84× under 32-bit SSE2.

This does not make `windows-sse2.exe` a bad build — it is still 1.12×
overall — but the Q5_K loop shape wants re-deriving per ISA tier
rather than one `#if defined(__SSE2__)` covering both.

### Headroom

matvec is **91.1%** of the forward pass on the 32-bit build and
**87.1%** on AVX2 — Amdahl ceilings of 11.2× and 7.75× respectively.
So the ladder has captured 1.37× of an available ~11×.

The reason is visible in the disassembly: the shipped AVX2 exe has 127
ymm references and 18 `vpmaddwd`, but **zero `vpmaddubsw`** — the
u8×s8 pairwise-multiply-accumulate that does 32 MACs per instruction
and is the natural primitive for these kernels. GCC will not generate
it from the portable C. That remains the single biggest identified
opportunity, unchanged by this measurement.

`lm head` is 20% of the forward pass in every build (11 calls, one per
generated token) and scales with the ladder like everything else.

### 1.14.0 → 1.18.1 on the default download: nothing. Zero.

The user also ran **1.14.0**, five minutes before 1.18.1, on the same
machine, same prompt, both 32-bit MMX. This is the comparison that
matters most and it is the least flattering one:

| metric | 1.14.0 | 1.18.1 | delta |
|---|---|---|---|
| generation tok/s | 2.133 | 2.152 | +0.89% |
| prompt tok/s | 3.311 | 3.318 | +0.21% |
| total wall s | 10.43 | 10.37 | −0.58% |
| matvec Mflop/s | 3672.7 | 3691.1 | +0.50% |
| matvec Q4_K s | 3.888 | 3.837 | −1.31% |
| matvec Q5_K s | 2.436 | 2.431 | −0.21% |
| matvec Q6_K s | 3.034 | 3.047 | +0.43% |

Run-to-run noise here is ~1%. **Every single delta is inside it.**
Four releases — 1.15, 1.16, 1.17, 1.18 — produced *no measurable
change whatsoever* for a Windows user who downloads the default
binary. Not a regression: simply nothing.

This is not surprising once stated plainly, and `git diff v1.14.0`
confirms it exactly. Across all that work the changed files are
`backend_vis.c` (SPARC only), `backend.c`/`backend.h`/`opts.c`/
`main.c` (the `--kernel` plumbing), and `infer.h` (version).
`backend_mmx.c` and `quant.c` are **byte-identical to v1.14.0**. The
32-bit Windows default is the MMX path, so of course it did not move.

Where the effort actually went:

- **1.15.x** — VIS kernels: SPARC only, and 1.15.2 turned Q6_K VIS
  *off* by default after real hardware said 0.8%.
- **1.16.0** — Makefile restructuring: no codegen change.
- **1.17.x** — `--kernel` selection, then **five consecutive releases
  of CI repair** (1.17.1–1.17.5, 1.18.1). Pure infrastructure.
- **1.18.0** — the ISA ladder. The first change since 1.14.0 that a
  Windows user can feel — and only if they pick a *different*
  download. The default `windows.exe` is untouched by design.

**The lesson is about where attention went, not about a bug.** Between
1.15 and 1.18 the dominant activity was fixing my own CI, and the
second largest was a SPARC kernel whose headline optimisation was
measured and rejected. The x86 hot path — 91% of the forward pass,
with an 11× Amdahl ceiling and a known missing instruction
(`vpmaddubsw`) — received no kernel work at all.

The honest framing for a release note is not "no regression". It is:
*if you are on the default Windows build, 1.18.1 is 1.14.0 with a
better build system.* The gains are real but they are all behind
choosing `windows64-avx2.exe`, and even that is 1.37×, not the
order-of-magnitude the Amdahl headroom permits.

`--backend i8` forced on the 32-bit build gives 1.034 tok/s against
mmx's 2.152 — **0.48×**, matching the 1.5.0/1.6.0 Geode ratio in
finding 37 almost exactly. The static priority list is right when the
compiler cannot vectorise, and finding 33's `I8_IS_VECTORISED`
override is right when it can. Both halves now confirmed on real
hardware.

## 47. VPMADDUBSW: the instruction the vectoriser will not find

Finding 46 measured 1.37x on the Windows ISA ladder against an Amdahl
ceiling above 7x, and identified the gap: the shipped AVX2 binary had
127 ymm references, 18 VPMADDWD and **zero VPMADDUBSW**. This is the
kernel that uses it.

| | MACs / instruction |
|---|---|
| PMADDWD (MMX) | 4 |
| VPMADDWD (GCC's autovectorisation of i8) | 16 |
| **VPMADDUBSW** | **32** |

The precondition is that one operand be *unsigned* bytes. After
unpacking, every k-quant weight is: Q4_K 0..15, Q5_K 0..31, Q6_K
0..63. The activations are already int8. GCC cannot exploit this
because the portable C promotes both to `int` before multiplying, and
nothing in the source says the weight is a small unsigned value.

Measured, same binary, `--backend` forced (sandbox Xeon):

| format | i8 | avx2 | speedup |
|---|---|---|---|
| Q4_K | 1.560 s | 1.344 s | 1.16x |
| Q5_K | 1.501 s | 0.868 s | 1.73x |
| Q6_K | 1.535 s | 0.817 s | 1.88x |
| end to end | 2.65 tok/s | 3.84 tok/s | **1.45x** |

**Q4_K gains least, and that is informative.** Its inner loop is a
single nibble mask, which GCC vectorises well on its own, so the
hand-written kernel is only recovering the difference between 16 and
32 MACs. Q5_K and Q6_K gain far more because merging their separate
bit-planes is control flow the vectoriser gives up on, while by hand
it is three shifts and a mask. **The value of a hand kernel is
inversely proportional to how well the loop already vectorises** --
which is not where instruction counting would have pointed.

### The horizontal sum was half the cost

The first working version reduced each 32-weight group immediately
with a per-vector hsum (two shuffles, two adds, an extract). Q4_K
gained only **1.12x** while Q6_K -- which reduces once per 128 weights
instead of once per 32 -- gained 1.90x. The reduction was costing
about as much as the multiply it followed.

Accumulating all eight groups of a 256-weight block into a register
array and reducing them together with two VPHADDD (`hsum4x8`) took
Q4_K to 1.16x and end-to-end from 1.44x to 1.46x on the forward pass.
**In a SIMD dot-product kernel, budget the reduction as carefully as
the multiply**; the arithmetic intensity of the tail is what decides
whether a wide instruction pays.

### Saturation, checked rather than argued

VPMADDUBSW saturates its int16 output. Findings 29 and 35 are both
cases where lane-budget reasoning was plausible and wrong, so this is
verified exhaustively by `tests/t_avx2sat.c`:

| format | wmax | worst lane | of 32767 |
|---|---|---|---|
| Q4_K | 15 | 3810 | 11.6% |
| Q5_K | 31 | 7874 | 24.0% |
| Q6_K | 63 | 16002 | **48.8%** |

Q6_K has the least margin and still sits at half the limit. The bound
is `2 * wmax * 127` because bk_quantize_x clamps activations to
[-127, 127].

### CPUID is not enough: the OS must opt in

AVX adds architectural state (the upper half of ymm), so the OS has to
promise to save it across context switches. If it has not, the first
VEX instruction raises #UD. **Windows XP never gained AVX support**,
so an XP machine on Haswell silicon reports AVX2 in CPUID and cannot
run it -- and this project ships an XP target.

The gate therefore checks CPUID.1:ECX OSXSAVE and AVX, then XGETBV for
XCR0 bits 1 and 2, then CPUID.7:EBX AVX2. With only the CPUID half,
`auto` would select a backend that crashes instead of falling through
to mmx. CI asserts `xgetbv` is present in the shipped exe.

### Selection: finding 33 needed qualifying

Finding 33 established "once the compiler can vectorise, prefer the
portable i8 over the hand-written kernel", from a table whose only
hand-written x86 entry was MMX at 4 MACs. At 32 MACs the conclusion
inverts. `auto` now tries avx2 first and falls back to the finding-33
rule unchanged. The i486 build defines neither `__AVX2__` nor
`__SSE2__`, compiles `backend_avx2.c` to nothing, and still selects
mmx -- verified 0 CMOV, 0 xmm/ymm.

## 48. 6.3 -> 14.4 tok/s: the kernel was never the bottleneck

Finding 47 shipped an AVX2 kernel worth 1.45x and treated that as the
result. The user's reply was that llama.cpp reaches ~15 tok/s on this
class of machine. That was correct and the gap was entirely ours.

| stage | tok/s |
|---|---|
| 1.19.0 as shipped | 6.35 |
| + thread pool | 9.10 |
| + llama.cpp kernel shape | 13.29 |
| + scale decode, dual accumulators, prefetch | **14.43** |

**Measure the machine before optimising the code.** The first useful
number was not a profile at all:

```
sequential read, 1 thread   10.3 GB/s
sequential read, 2 threads  20.9 GB/s
model streams 381 MB/forward pass
```

At 6.35 tok/s we were moving 2.7 GB/s of a 20.9 GB/s budget. That said
plainly: not memory-bound, 7x of headroom, and the problem is compute
or parallelism. Every later decision followed from that one
measurement, and it took thirty seconds.

### The thread pool was the single biggest win, and it was absent

There was **no threading anywhere** -- one core of two. Rows of a
matvec are independent, so `q_matvec` splits the row range across a
persistent pool. Bit-identical by construction: rows are split, never
dot products, so no partial sum crosses a thread.

Two hazards that a naive version gets wrong:

- **The shared activation buffer.** Every worker calls
  `bk_quantize_x` for the same vector and would race writing one
  static scratch. The first fix guessed whether x had changed
  (pointer + sampled endpoint values) -- unsafe, because x lives in a
  buffer rewritten in place, so a false hit silently computes with
  stale activations. Replaced with an explicit `bk_quantize_hold()` /
  `release()` latch around the parallel region. **When the cost of a
  wrong guess is a silent wrong answer, do not guess.**
- **Per-head scratch.** The DeltaNet recurrence wrote `kv_delta[hd]`,
  one shared row. Threading the head loop needs `[n_heads][hd]`.

### The kernel shape mattered more than the instruction

Reading `ggml_vec_dot_q4_K_q8_K` showed the real difference, and it was
not VPMADDUBSW -- we already had that. Per 256-weight superblock:

| | ours (1.19.0) | llama.cpp |
|---|---|---|
| horizontal sums | 8 | 0 |
| scalar float ops | ~32 | ~2 |
| float converts | 8 | 1 |

Three things make their shape possible, and they are a package:

1. **A Q8_K-style activation plane** -- one scale per 256 values, not
   per 32. Nothing else works without it.
2. **The weight scale applied in the integer domain**, folded into the
   `VPMADDWD` that had to widen int16->int32 anyway. Free.
3. **One int32 accumulator per superblock.** Per-superblock is also
   what keeps it safe: a 248320-column row accumulated in int32 would
   overflow; per superblock the worst case (Q6_K) is 66x inside.

Afterwards the disassembly still showed 25 `vmulss` + 17 `vcvtsi2ss`:
the k-quant minimum term. Also integer, also accumulated as an int and
converted once. **Vectorising the multiplies just moves the bottleneck
to whatever scalar arithmetic was hiding behind them** -- re-read the
disassembly after every win.

### Isolated benchmarks are not optional, and neither is picking the
### right shape for them

`tests/t_kbench.c` was written mid-way because the whole-model profile
kept disagreeing with the kernel edits. It immediately showed the new
Q4_K kernel at **0.73x -- slower than portable i8** at ncols=3584
nrows=64, which nearly caused a revert. At nrows=2048 the same code was
**1.33x faster**. The small-nrows shape fits in L3 and measures a cache
resident loop the model never runs; the model's real matrices are
1024x3584 and larger.

Final isolated speedups vs portable i8 at 1024x3584:

| format | speedup |
|---|---|
| Q4_K | 1.94x |
| Q5_K | 3.08x |
| Q6_K | **4.54x** |
| Q8_0 | 1.05x |

Q6_K gains most for the reason finding 47 identified: the more
bit-plane merging a format needs, the less the vectoriser can do and
the more a hand kernel is worth.

### Giving up bit-identity, on purpose, with a replacement test

The new shape is not bit-identical to the scalar path: activations are
quantised per 256 not per 32, the sum is reassociated, and the scale is
applied before the float conversion. All legitimate; none exact.

`t_ident` therefore no longer covers these kernels, and dropping a test
without replacing it is how correctness quietly rots.
`tests/t_avx2acc.c` measures relative L2 error against the **float
reference** and requires AVX2 to be no worse than portable i8:
measured 2.2e-03 - 8.0e-03 against i8's 2.8e-03 - 5.7e-03, with Q4_K
at 1024 columns actually more accurate than i8.

**The rule:** an optimisation may weaken a guarantee, but it must
replace the test that guarded it with one that bounds the new
behaviour. "It is approximately right" is a claim, not a check.

## 49. Streaming was no slower than cache-resident, so the limit was
##     uops, not bandwidth

Recorded late: findings 48 and 50 were written at the time and this
one was not, leaving two citations in `backend_avx2.c` pointing at a
number that existed only in the 1.20.0 changelog. Written up here so
they resolve.

The question after 1.20.0 was whether the AVX2 kernels were limited by
memory or by instruction issue. The two need opposite work — fewer
bytes versus fewer uops — so guessing wrong wastes a release.

The discriminating measurement is to run the same kernel on a matrix
that fits in cache and on one that cannot possibly fit, and compare
**per-weight** time:

```
cache-resident (1024x64)     : 0.111 ns/weight
streaming      (1024x248320) : 0.072-0.103 ns/weight
```

The streaming case is the lm head, 209 MB against a 54 MB L3 — four
times the cache. It came out **no slower** than the cache-resident
case. If bandwidth were the constraint that is impossible.

Cross-checked against the machine: it sustains 11.3 GB/s aligned and
10.3 GB/s unaligned on a single core, while the model at that speed
was asking for only ~5.6 GB/s. Half the available bandwidth was going
unused.

**Conclusion: the inner loop was issue-bound.** That is what justified
the next round being instruction selection — the two-row kernels
(finding 48, item 7) and later the word-parallel scale decode (finding
53) — rather than anything that moves fewer bytes.

The prefetch sweep belongs to the same measurement. Weights are
streamed once and never revisited, so the hardware prefetcher has no
history at a row boundary. One superblock ahead is ~26 ns of work
against an 80-100 ns DRAM latency, so the line lands too late:

```
distance  2 -> 10.7 tok/s
          4 -> 11.1
          8 -> 11.6
         24 -> 12.0
         48 -> 11.4
```

Past ~24 the prefetches evict each other. (1.22.0 re-swept this after
the scale-decode work moved the balance and settled on 32; see finding
53.)

**Caveat now attached to the headline number.** "0.102 ns/weight for
every format regardless of bits per weight" was true of the 1.20.0
kernels and was the cleanest evidence for the uop argument. It is no
longer true: after finding 53 the formats separate (Q4_K 0.056, Q5_K
0.066, Q6_K 0.071), because the scale decode that dominated them all
equally is gone. The conclusion survives, the specific figure does
not.

## 50. An ISA ladder was the wrong shape; a gated object is the right one

1.18.0 shipped four Windows downloads (`-sse2`, `64`, `64-avx2`, plain)
by moving the COMPILER BASELINE per artifact. 1.21.0 replaces all of it
with one binary per platform that carries every kernel and picks at run
time. The mechanism was already in the tree -- `backend_mmx.c` has
worked this way since 1.0 -- it just was not applied to AVX2.

Why the ladder was worse, concretely:

- it made the user diagnose their own CPU;
- a wrong guess died with an illegal instruction, not a slowdown;
- the compiler-vectorised SSE2 tier was never worth an artifact
  (finding 46 measured it at 1.12x while costing a whole download);
- and AVX needs OS support, so `-avx2` on Windows XP faults even on
  silicon that has it.

**The mechanism.** `backend_avx2.c` is compiled ALONE with `-mavx2`
into its own object; everything else stays at the i486 baseline; the
linker puts them together. Result, verified on the shipped 32-bit file:

```
ymm=952  vpmaddubsw=64  xgetbv=9  mmx=272
qemu -cpu pentium2/pentium3/core2duo -> selects mmx
qemu -cpu Haswell                    -> selects avx2
```

One binary, boots on a 486, uses VPMADDUBSW on a Haswell.

### Three traps, all of which bit

**1. `__AVX2__` is false in the file that registers the backend.**
The table gated its entry on `#if defined(__AVX2__)`. Once `backend.c`
moved to the baseline that macro is false, so the kernel was linked in
and never offered -- a silent 0% "optimisation". Registration must key
on the *intent* macro (`INFER_HAVE_AVX2`), never on the *codegen* macro.
CI now asserts the shipped 32-bit binary lists `avx2`.

**2. Intrinsics leak into baseline files.** Two AVX2 blocks had grown
inside `backend.c` (activation quantisation) and `qwen35.c` (the
DeltaNet recurrence). Both had to move behind `bk_a2_*` entry points
that return 0 when unavailable, so the baseline caller keeps its scalar
loop and needs no `#ifdef`.

**3. Every target that does NOT link the AVX2 object still needs the
symbols.** SPARC, s390x, `NO_AVX2=1` and several standalone test
binaries all broke with `undefined reference to bk_a2_amax`. The
fallbacks live in `backend_a2stub.c`, which is always compiled, guarded
by `INFER_A2_LINKED` -- a macro the *Makefile* defines exactly when
`backend_avx2.o` is in the link. It cannot be `__AVX2__`: the stub is
compiled at the baseline, where that is false even in a build that does
link the real helpers. Getting this wrong produces either duplicate
symbols or missing ones, and I hit both before writing it down.

### What the CI assertions become

The old per-tier ISA checks are meaningless now. Three replace them:

- every artifact must have **>100 ymm AND >100 MMX AND an xgetbv** --
  if the two-stage compile ever collapses into one command, either the
  baseline is contaminated or AVX2 vanished, and one of those fails;
- every `src/*.c` except `backend_avx2.c` must compile to 0 CMOV and
  0 SSE/AVX at `-march=i486`;
- "no unconditional SSE/AVX" is now checked against a `NO_AVX2=1`
  rebuild, because the normal binary is *supposed* to contain ymm.

**The rule:** ship one artifact per platform and select at run time.
A build flag that changes the instruction floor is a support burden
disguised as an optimisation -- put the fast code in its own gated
object instead, and let the CPU decide.

## 51. MMX Q6_K: two passes over the same bytes, and the MMX floor

Q6_K measured **0.601 ns/weight against Q4_K's 0.293** while being 42%
of this model's matvec time. The cause was structural, not scheduling:
`mmx_q6K_half` handled the low-nibble and high-nibble planes in
separate passes, re-loading and re-unpacking the same `ql` and `qh`
bytes each time.

| format | MMX instrs / weight | ns/weight |
|---|---|---|
| Q4_K | 1.19 | 0.293 |
| Q5_K | 1.88 | 0.384 |
| Q6_K before | 3.50 | 0.601 |
| Q6_K after | 2.12 | 0.420 |

**Instruction count predicted the ratio almost exactly** (3.50/1.19 =
2.94 against a measured 2.05), which is the opposite of finding 36,
where the i486's cost model was *not* instruction count. On this
in-order-ish MMX path it is.

Q4_K and Q5_K already used the merged shape -- one `ql` load, `PAND`
for the low nibble, `PSRLW`+`PAND` for the high, all masking in the
byte domain, one widening at the end. Q6_K did not, only because its
2-bit high field needs a different shift per plane. That is two
instructions, not a second pass.

### The byte-domain vs word-domain mask trap

`c_mask_3` is `0x0003000300030003` -- a **word** mask. The rewritten
kernel masks *packed bytes* before widening, so it needs
`0x0303030303030303`. The old kernel got away with the word constant
because it unpacked first. Any MMX code that masks in the byte domain
needs a byte-domain constant, and the file already had `c_mask_fb`
alongside `c_mask_f` for exactly this reason -- the 2-bit companion
was simply missing. Symptom was a silently wrong Q6_K, caught by
`t_ident`'s memcmp.

### Two plausible optimisations that measured worse

- **Halving the reduction rate.** One `MOVD` per 32 weights instead of
  per 16, with two independent accumulator chains: **0.601 -> 0.598**.
  Nothing. The store traffic was never the bottleneck, and the
  dependency chain was already short enough to hide.
- **The VIS bias trick.** `sum((w-32)*x) == sum(w*x) - 32*sum(x)`
  removes four `PSUBW` per 16 weights, and `xq->s16` already holds the
  sums. Kernel improved 0.420 -> 0.409; **the whole model got slower,
  3.173 -> 3.129 tok/s.** The correction adds a multiply and a
  subtract per 16-weight group to the scalar float phase, and Q6_K has
  eight groups per 128 weights, so the float side lost more than the
  MMX side gained. The same trick is a win on VIS (finding 15.0)
  because that kernel's float phase is shaped differently.

### Where the MMX floor actually is

matvec is 93.2% of the forward pass and uses **1.43 GB/s of ~10 GB/s
available** -- instruction-bound, not memory-bound, and already
issuing ~1.6-2 MMX instructions per cycle. Only instruction count is
left.

Q4_K's 19 instructions per 16 weights break down as 5 nibble extract,
**6 widening**, 4 `PMADDWD`, 4 `PADDD`. The six `PUNPCK` exist only
because `PMADDWD` consumes words while the weights arrive as packed
nibbles. MMX has no unsigned-byte multiply-accumulate: `PMADDUBSW` is
SSSE3 (2006), and Extended MMX -- the newest thing a Geode LX has --
adds `PSHUFW`, `PMAXSW`, `PAVGB`, `PSADBW`, `MOVNTQ`, `PREFETCH` and
nothing that helps here.

Weighted by each format's share of the model's weights (Q4_K 46.6%,
Q5_K 23.0%, Q6_K 30.4%), the kernels now average **1.63 instr/weight**.
Driving Q5_K and Q6_K all the way to Q4_K's 1.19 would be 1.37x, or
about **4.35 tok/s** -- and that is the optimistic bound assuming
perfect kernels. **Further MMX gains have to come from outside the
inner loop**, which is what makes lm-head requantisation (25% of the
pass, in the most expensive format per weight) the next lever rather
than more instruction-shaving.

## 52. `static` scratch + a thread pool = silent corruption, and four
## test layers that all missed it

1.20.0 added row-parallel matvec. The MMX kernels kept their per-row
integer accumulator in a `static` array -- chosen originally to keep
1 KB off a 486's stack -- so every worker wrote the same buffer. The
model produced fluent gibberish; `i8`, `avx2` and `ref` were fine
because they accumulate in locals.

**Every individual dot product was still correct.** The rows raced
through one scratch array on the way out. That is what made it
invisible:

| check | why it passed |
|---|---|
| `t_ident` (memcmp, all formats) | single-threaded |
| synthetic sweep of every (format, ncols, nrows) | single-threaded |
| instrumented build comparing `i8` vs `mmx` on every real call | running `i8` first serialised the pair |
| `t_thread` | went through `q_matvec`, which uses whatever `auto` picks -- `avx2` here. **MMX was never threaded by any test.** |

The third one is the most instructive: adding the cross-check *made
the bug disappear*, because it introduced an ordering the buggy code
depended on. A debugging tool that perturbs the thing it measures will
happily prove the wrong conclusion.

**The rule that would have caught it:** a test that exercises "the
default configuration" tests one point in the matrix. If a property is
claimed for N backends, the test must loop over N backends by name.
`t_thread` now does, and with the `static` restored it reports six
nondeterministic configurations -- so the test demonstrably has the
power to catch the bug it was written for.

### Secondary fixes

- The `MMX_ROW_ACC` bound was enforced only by a comment. `qmv_mmx`
  now computes the requirement and falls back to `qmv_i8` if a tensor
  would exceed it, rather than smashing the stack.
- Threading became **opt-in** (`--threads` defaults to 1). Defaulting
  to one-thread-per-core meant users who never asked for concurrency
  still got it, and here that meant silent corruption. It also bought
  nothing on the 2-core test box: 14.48 -> 14.41 tok/s, because the
  memory bus was already saturated at one thread.

**The wider lesson:** `static` local buffers are a single-threaded
idiom. Introducing a thread pool invalidates every one of them at
once, and the compiler will not say a word. When adding concurrency,
grep for `static` in every function the new parallel region can reach
-- that search takes a minute and would have found this before it
shipped.

Done for this tree: the only remaining `static` scratch is in
`bpe_word` (`src/tokenizer.c`), and the pool has exactly two worker
bodies -- `mv_slice` and `dn_recur_slice` -- neither of which can
reach the tokenizer, because tokenisation completes before the first
forward pass. It is safe, and now on record as *checked* rather than
assumed.

## 53. The scale decode cost more than the arithmetic it fed

Chasing the 2x gap against llama.cpp. The AVX2 Q4_K kernel was at
0.070 ns/weight. Stripping it down and timing each layer, on the same
data:

| what | ns/weight |
|---|---|
| loads only | 0.015 |
| + nibble split | 0.016 |
| + VPMADDUBSW / VPMADDWD | **0.019** |
| the full kernel | **0.070** |

**73% of the kernel was not the arithmetic.** That is not a ratio you
can guess at from reading code -- the vector instructions are what the
eye lands on, and they were a fifth of the cost.

Timing the remaining pieces individually found `a2_scales8` at
**0.038 ns/weight** -- twice the vector work it feeds. It unpacks six
6-bit scales and six 6-bit mins from twelve packed bytes, once per
superblock per row, which on the 248320-row lm head is a million calls
per token. The old form walked the eight indices with a branch and
several shifts each; the new one treats the twelve bytes as four 32-bit
words, three masks, no branch. **9.76 ns -> 1.94 ns, 5.0x.**

Result: Q4_K 0.070 -> 0.056, Q5_K 0.086 -> 0.066, Q6_K 0.085 -> 0.071
ns/weight.

**The rule:** when a kernel is slower than its instruction mix
suggests, time it *by subtraction* -- build the loop up one stage at a
time and measure each. Reading the assembly tells you what is there;
subtraction tells you what it costs. Here the assembly showed 64
VPMADDUBSW against 80 VPBROADCASTW and I spent a while on the
broadcasts, which turned out to be worth only 7%.

### Three hypotheses that were wrong, and how each died

Recorded because each was plausible, cheap to test, and would have been
expensive to "fix" on faith.

**Transparent hugepages.** The benchmark used `malloc` (THP-eligible)
while the model uses a file mapping, where THP is off. A 384 MB
streaming read measured: anon+`MADV_HUGEPAGE` 10.91 GB/s, anon 4K
11.78, **file mmap 12.58**. The file mapping was *fastest*. Dead.

**Cache eviction by the DeltaNet state.** 18 layers x 16 heads x
128x128 floats = 18.9 MB, 35% of L3, read and written twice per token.
Adding a 1 MB state touch between matvecs in a benchmark changed
6.14 -> 7.16 GB/s -- it got *faster*, i.e. noise, not interference.
Dead.

**Prefetch overshooting the row.** `A2_PFDIST=24` prefetches 24
superblocks ahead; a 1024-column Q4_K row is only 4 superblocks, so the
prefetch lands six rows away. That looked like obvious waste. Sweeping
the distance in-model: 8 -> 12.8 tok/s, 16 -> 14.6, 24 -> 14.6,
32 -> 14.7, 48 -> 14.7, 64 -> 14.2. Overshoot **helps**, because rows
are contiguous in memory and the next row is read immediately after.
Dead -- and the distance was raised to 32 rather than lowered.

### Where the remaining gap is, measured

- Machine sustains **11.3 GB/s** single-core streaming (AVX2 loads,
  prefetched, 4 accumulators -- all three variants within 2%).
- The model moves **520 MB per generated token** (508 MB file, and the
  profiler's own byte counter agrees).
- Hard ceiling: **21.7 tok/s**. We are at 14.7 = **7.6 GB/s, 67%**.
- The same kernel on a cold 522 MB mmap'd file with the model's exact
  shapes reaches **9.3 GB/s**; inside the engine the same shapes run at
  6.9-8.2.

So roughly a fifth of the remaining gap is inside the kernel and the
rest is engine-side, and it is *not* any of the three causes above.
The next candidate is the 186 separate matvec calls per forward pass,
each restarting the hardware prefetcher on a different tensor -- but
that is a hypothesis, not a finding, and the previous three teach that
the difference matters.

## 54. The thread pool spent more time sleeping than the kernels spent
##     working

Reported from SPARC: ~19% of the process in the kernel, cores no
longer pegged. Reproduced on x86 immediately — `-T 2` on two cores was
using **CPU% 159** instead of ~200, with **17,402 voluntary context
switches** over 58 forward passes (~300 per pass).

The cause is a mismatch that grew over time rather than a bug that was
introduced. `tpool.c` was written when a `tp_parallel_for` call had
"roughly a millisecond of work to give away" (its own comment). Since
then the kernels got several times faster and the work per call
shrank, but the barrier did not: every call was a condition-variable
round trip in both directions.

Measured with the real `tpool.c` and an empty job:

```
tp_parallel_for, one barrier : 44-48 us
```

A forward pass issues ~95 of them (77 matvecs with `nrows >= 64`, plus
18 DeltaNet head loops). Small shapes were slower threaded than
serial:

```
nrows=64    serial 19.8 us   pooled 51.0 us   0.39x
nrows=256   serial 79.9 us   pooled 92.1 us   0.87x
nrows=1024  serial 318 us    pooled 216 us    1.47x
nrows=4096  serial 1276 us   pooled 683 us    1.87x
```

Fix: poll a generation counter before blocking, and have the joiner
poll per-worker completion stamps instead of sleeping on a condvar.

```
                              before    after
tp_parallel_for, one barrier  44-48 us  0.25 us   (~190x)
nrows=64                      0.39x     1.93x
nrows=4096                    1.87x     2.01x
```

Model, 2 cores: **17.25 -> 22.16 tok/s (+28.5%)**, CPU% 159 -> 181,
nvcsw 17402 -> 4510. `-T 1` unchanged, as it must be — the pool is not
used at all.

### The part that was nearly a regression

Spinning unconditionally is wrong. With more threads than cores the
spinners steal CPU from the workers being waited on:

```
        1.22.0   naive spin
-T 4    17.2     10.4 tok/s
-T 8    16.9      7.6 tok/s
```

Putting `sched_yield()` in the spin loop only softened it (`-T 4`:
15.3, still below baseline). The working answer is to not spin at all
when oversubscribed: the spin budget is set from
`nthreads <= tp_cpu_count()` in `tp_start`, and at zero both the
worker and the joiner take the original blocking path, which restores
the old behaviour exactly rather than approximating it. After that,
`-T 4` and `-T 8` are within noise of baseline.

**Lesson:** a fast path that is only fast under an assumption must
detect when the assumption fails and fall back to the code it
replaced, not to a degraded version of itself.

### Portability notes

- `sched_yield()` is in librt on Solaris, not libc. Behind
  `TP_YIELD()`: `Sleep(0)` on Win32, `sched_yield()` under
  `_POSIX_PRIORITY_SCHEDULING`, no-op otherwise. No new link
  dependency anywhere.
- Both pthread and Win32 paths were changed, so Windows benefits too.
- x86 and SPARC are TSO; `volatile` plus a compiler barrier is what
  these store/load pairs need. `t_thread` cross-built for sparc64
  passes under qemu at 1/4/8 threads.
- Output is bit-identical at every thread count and equal to the
  previous release: this is scheduling only, no arithmetic changed.

## 55. The same barrier, two platforms, one `while` and one `if`

Finding 54 rewrote the pool barrier to spin before blocking, on both
the pthread and Win32 paths. The pthread version re-checked its
predicate in a loop. The Win32 version used a bare `if`.

That is the classic condition-variable mistake, and Win32 auto-reset
events make it worse rather than better. The dispatcher signals
`tp_go` on **every** batch, whether or not the worker is blocked. A
signal delivered while the worker is spinning is still latched, so the
next `WaitForSingleObject` returns at once with no new work. With an
`if` the worker fell through, read an unchanged `tp_sgen`, re-ran the
**previous** slice and stamped `tp_sdone` with a stale generation —
after which the dispatcher either waited forever for a stamp already
written, or proceeded while workers were still writing `y[]`.

On real hardware it crashed at `-T 2`. `-T 1` never enters the pool,
so the default was safe and it shipped in 1.23.0 and 1.23.1.

Modelling the auto-reset event exactly, 400 batches, one worker:

```
CURRENT (if)     stale-wakeups=10   batches with wrong slice: 10
FIXED (while)    stale-wakeups=0    batches with wrong slice: 0
```

A latent second defect sat in the same function: `tp_start` created
event `i` then immediately spawned worker `i`, so a worker could wait
on a handle while later ones were still being created, and the
allocation-failure path left running workers with an id `>= tp_n`,
never scheduled and never joined. Events are now created in one pass,
threads in a second.

### Why four rounds of testing missed it

Every threading test runs on Linux, where the pthread path is right.
`t_thread` passed at 1/4/8 the whole time. The Windows binaries had
never been run — a gap acknowledged since 1.18.0 and repeatedly
justified with "no Wine in the sandbox".

**That justification was wrong.** Wine installs fine and runs both
binaries against the real model. The gap was never technical.

### The rules this produces

- **A platform you cannot execute is a platform you cannot claim.**
  Four releases of "verified" Windows binaries rested on static
  inspection of imports and opcode counts. None of that can see a
  logic error in a wait loop.
- **When one code path is ported to another, diff the control flow,
  not just the API calls.** `pthread_cond_wait` → `WaitForSingleObject`
  looks like a faithful translation and is not, because the loop
  around it carries the correctness.
- **Guard the source shape when no test can reach the behaviour.**
  CI now asserts every go-signal wait is loop-governed on both paths.

## 56. SPARC: MADV_SEQUENTIAL means something different on Solaris

Reported from a real UltraSPARC: 25.88 s/tok generation, and "VERY
high disk usage" throughout.

The disk usage is the interesting half. `sys_map_advise_random()` has
advised `MADV_SEQUENTIAL` since finding 48 replaced the original (and
genuinely wrong) `MADV_RANDOM`. On Linux that widens readahead and
nothing else -- pages are **not** dropped behind the read pointer,
confirmed on lkml: it "will only increase the amount of read ahead ...
but will not influence the rate at which the pages just read will be
freed from memory".

Solaris does not agree. Its manual says of `MADV_SEQUENTIAL`: pages
"will be accessed only once and can be freed behind the current access
point". That is licence to discard.

The access pattern here is sequential **within** one token and then
starts over from the beginning for the next one. So the hint is true
for a few milliseconds and false forever after: the pages the next
token needs are exactly the ones Solaris was just told to throw away.
On a machine with less RAM than the 508 MiB model, every generated
token re-reads the model from disk.

`MADV_WILLNEED` alone gives the useful half -- start pulling the
mapping in -- with no licence to evict. Neither hint forces residency,
so a small machine still works; it just pages honestly.

**Lesson:** a POSIX advice constant is not a portable API. The name is
standard, the semantics are per-kernel, and this one differs in the
direction that matters on exactly the platform least able to absorb
it.

## 57. SPARC has no integer-to-float register move, and -O3 makes it worse

Same machine, the compute half. The VIS backend was reaching ~56
Mflop/s where the model needs ~100 to hit 15 s/tok.

Disassembling `vis_dot_q4_K` (sparc64 gcc 14, `-O3`) showed **32
`fmul8x16` against 105 `ld`, 64 `st`, 51 `ldx` and 31 `stx`** -- about
250 memory operations for 32 multiplies. 99 of 103 stores were to
`%fp`, i.e. stack spills, not real work.

The cause is architectural. VIS operands live in the **floating-point**
register file, and UltraSPARC has no direct move between the integer
and FP files: every integer-side value fed to a VIS instruction must
go out to memory and come back. `PACK_U8X4` -- which turns four packed
nibbles into a VIS vector -- is therefore one store plus one load,
every time, and there are four per 32-weight group.

`-O3` unrolls the inner loop 4x, multiplying the number of such values
live at once, and the allocator spills. Normalised per `fmul8x16`, so
the unroll factor cannot flatter the count:

```
                     insns/mul   stack-spill ops/mul
  -O3                   19.5            4.2
  -O2                   46.0           10.5   (barely unrolls)
  -Os                   38.8            9.2
  -O2 -funroll-loops    16.0            3.7   <- best on both
```

Same ordering on `vis_dot_q5_K` (27.0/7.1 -> 21.5/4.7). The Solaris
GCC target now defaults to `-O2 -funroll-loops`, overridable with
`SOLGCCOPT`.

**Lesson:** `-O3` is not a synonym for "faster" on a register-poor
machine with an expensive register-file crossing. Normalise by the
unit of real work before comparing, or unrolling will flatter itself.

## 58. Eight function calls per super-block, for a single array read

While measuring finding 57, `i8_dot_q6_K` on sparc64 showed **9 calls
per Q6_K super-block**: one to `rd_f16p` and **eight to
`sum16.isra.0`**. `sum16()` is one line:

```c
static int sum16(const bk_qx *xq, long idx) { return xq->s16[idx / 16]; }
```

At `-O2` GCC declined to inline it, so each of the eight epilogue uses
became a real call -- a register-window save and restore on SPARC, in
a kernel that is **35% of a token** on that machine (Q6_K is the LM
head).

Adding an `always_inline` attribute to `sum16()` and `rd_f16p()`:

```
  i8_dot_q6_K   328 -> 272 insns,  105 -> 54 spills,  9 -> 1 calls
```

Doing the same to `get_scale_min_k4()` was tried and is **worse**: it
is big enough that inlining it twice per group inflates Q4_K 256 ->
288 and Q5_K 176 -> 216, and the share-weighted total across the three
formats goes 219 -> 239. Reverted.

**Lesson:** "hot" is not the criterion for force-inlining; "small and
hot" is. And check the whole weighted profile, not the one kernel you
were looking at.

## 59. The VIS Q6_K kernel is still not worth enabling

Finding 30 disabled the VIS Q6_K kernel because it measured no faster
than the portable path. Re-checked while chasing finding 57, since
Q6_K is 35% of a SPARC token and the biggest single target.

Per 256-weight super-block, `-O2 -funroll-loops`, whole function:

```
  i8_dot_q6_K       328 insns, 105 spills
  vis_dot_q6_K_al   440 insns, 127 spills
```

VIS is **1.34x more instructions**, which matches the user's own
`--kernel bench`: q6k vis 0.711 ms vs i8 0.712 ms, i.e. identical,
because the runtime picks `i8` for q6k anyway.

The reason is specific to the format. A Q4_K weight is one nibble and
needs one mask. A Q6_K weight is split across two planes -- four bits
in `ql`, two in `qh` -- so reassembling it costs mask, shift and or
per weight, all on the integer side, and every reassembled value then
has to cross the integer-to-FP gap that finding 57 describes. The
format defeats the instruction.

Still compiled out. `-DINFER_VIS_Q6K` remains for anyone who wants to
re-measure on different silicon.

> **RETRACTED (1.25.0).** Someone did re-measure on the silicon, and
> the kernel is worth **−29% on `matvec Q6_K`** and **13.87 → 9.48
> s/tok**. The instruction counts above are correct and were verified
> again on gcc 14.2; the conclusion drawn from them is not. Counting
> instructions treats an `smul` and an `add` as one unit each, and the
> whole point of this kernel is that it trades 8 multi-cycle integer
> multiplies for 16 pipelined `fmul8x16`. See **finding 61**.

## 60. The SPARC wins do not transfer to x86, and the reason is the call

After 1.24.0 took SPARC from 25.88 to 13.87 s/tok, the obvious question
was whether i486/MMX/Windows had the same bottlenecks. Checked all
three, one at a time.

**The `madvise` bug does not exist on Windows.** The Win32 path maps
with `CreateFileMapping`/`MapViewOfFile` and opens the file with
`FILE_FLAG_RANDOM_ACCESS`, which looks like the `MADV_RANDOM` mistake
of finding 48. It is not, for a reason worth writing down: per
Microsoft, "memory-mapped file access does not go through the cache
manager, and consequently these cache hints have no effect". The
weights are read only through the mapped view, so the flag never
touches the hot path. (It does ask the cache manager to keep mapped
views resident, which Microsoft discourages in general but which is
mildly in our favour here.) On Linux the 1.24.0 change applies but
measures as nothing, because Linux never freed behind us.

**The inlining win is real in the object code and invisible in the
clock.** Force-inlining `sum16()`/`rd_f16p()` (finding 58) helps every
target's instruction count, including the shipped Windows 32-bit
build:

```
                       q4_K      q5_K      q6_K     share-weighted
  before (mingw)    152/4c    159/4c    330/9c          200
  after  (mingw)    151/4c    162/4c    289/1c          186   (-7%)
```

End to end on x86, 32-bit, single thread, 12 tokens, three
interleaved pairs:

```
  i8    before 1.418 1.420 1.379   after 1.400 1.421 1.405
  mmx   before 3.099 3.101 3.046   after 3.041 3.057 3.050
```

Nothing. A 7% instruction reduction bought 0%.

**Why.** The saving is eight *function calls* per Q6_K super-block. On
SPARC a call is a `save` that rotates a register window, and a window
overflow traps into the kernel to spill sixteen registers; the machine
is in-order and single-issue, so that latency is fully exposed. On
x86 a call is a push and a jump, with no window and no trap, and any
out-of-order core hides it entirely.

**Lesson:** an optimisation is only as portable as the *cost model*
that motivated it. "Fewer instructions" is not a cost model. This is
finding 36 ("instruction count is not the i486's cost model either")
arriving from the opposite direction: the i486 turns out to be much
closer to x86 than to SPARC on precisely this axis.

The change is kept because it is free, harmless, and worth 7% of the
instruction stream on the machine that cares.

## 61. Instruction counts cannot arbitrate between two functional units

Findings 30 and 59 both rejected the VIS Q6_K kernel. Both are wrong.
A contributor with an UltraSPARC IIi enabled it by default (PR #3) and
measured, on the machine:

```
                    1.24.0      1.25.0     delta
generation        13.87 s/tok  9.48 s/tok  -31.7%
prompt             7.55        6.93         -8.2%
matvec throughput  126.0       162.5 Mflop/s +29%

per call (us), normalised so 27 vs 52 passes cannot flatter either
  matvec Q6_K     272985      193651       -29.1%
  lm head        7777033     3746806       -51.8%
  feed-forward    144643      122446       -15.3%
  attention        82010       82152        +0.2%   <- control
```

The **10 s/tok goal that had been open since 1.24.0 is met.**

### Why the measurement is trustworthy

Attention carries no Q6_K, and it moved +0.2%. That is the control
finding 30 itself demanded ("always check a stage nobody touched"),
and it rules out the session contamination that poisoned 1.15.1.

The arithmetic also closes: Q6_K saves 1.246 s per forward pass, and
the total pass moved 1.247 s. A change that recompiles exactly one
kernel should account for the whole delta and nothing else — it
accounts for 99.9%. Q4_K drifted the *wrong* way by 5%, which is what
honest noise looks like.

### Why finding 59 got it backwards

The counts in finding 59 are real; re-derived on gcc 14.2:

```
                 insns  smul  fmul8x16
i8_dot_q6_K        272     8         0
vis_dot_q6_K_al    440     0        16
```

VIS is 1.6x the *instructions* and that is why it was rejected. But
`i8_dot_q6_K` issues **8 integer multiplies** per super-block and the
VIS kernel issues **none**, replacing them with 16 pipelined
`fmul8x16`. Sun's own UltraSPARC manual describes "a multi-cycle
integer multiplier" against an FPU where "most instructions are fully
pipelined, with a throughput of one per cycle". Counting each
instruction as one unit hides exactly the trade the kernel exists to
make.

The `--kernel bench` number that seemed to confirm it (`q6k vis 0.711
ms vs i8 0.712`) measured the wrong shape: a 512-column synthetic
row-block is dominated by per-call fixed cost, while the real Q6_K
consumer is a 248320-row lm head. **The bench is not a proxy for the
lm head, and should not be read as one.**

### The rules this produces

- **Static instruction counts screen; they do not arbitrate.** They
  are fair between two kernels using the same functional units. Between
  the integer multiplier and the FP pipe they are meaningless. On a
  machine with no performance counters, a whole-run measurement with an
  untouched control stage outranks any count.
- **"Inconclusive" is not "no gain".** Finding 30's data could not
  size the change (+8% to −4% under plausible corrections). It was
  recorded as a rejection and inherited as settled fact for nine
  releases.
- **A gated kernel needs a gated test.** `vis-shim-test` built
  `backend_vis.c` without `-DINFER_VIS_Q6K`, so its Q6_K cases silently
  exercised `qmv_i8` instead. Verified by breaking the VIS kernel's
  bias term: the old command line reported `0.000e+00 ok` on a kernel
  that was provably wrong, while the fixed one fails with ~9.5e-02.
  Both shim branches now pass the define.

## See also

- [../PERFORMANCE-ANALYSIS.md](PERFORMANCE-ANALYSIS.md) — the full
  optimisation record, including every rejected approach
- [ARCHITECTURE.md](ARCHITECTURE.md)
