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

## See also

- [../PERFORMANCE-ANALYSIS.md](PERFORMANCE-ANALYSIS.md) — the full
  optimisation record, including every rejected approach
- [ARCHITECTURE.md](ARCHITECTURE.md)
