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
`-DINFER_VIS_Q6K` (`make solaris-vis-q6k`) and is **off by default**,
because the default should be the path that measured faster.

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

## See also

- [../PERFORMANCE-ANALYSIS.md](PERFORMANCE-ANALYSIS.md) — the full
  optimisation record, including every rejected approach
- [ARCHITECTURE.md](ARCHITECTURE.md)
