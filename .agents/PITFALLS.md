# Pitfalls

Bugs that actually happened in this repository, what they looked like,
and how they were found. Every one of these cost real time.

The pattern worth internalising: **the expensive bugs here do not
crash.** They produce a program that builds cleanly, loads the model,
prints correct metadata, runs at full speed, and returns confident
nonsense.

Full write-ups with measurements are in
[`../docs/FINDINGS.md`](../docs/FINDINGS.md).

---

## 1. Byte order guessed wrong → eight minutes of garbage

**Symptom:** `!!!!!!!!!!` after eight minutes on an UltraSPARC. Perfectly
healthy-looking profile: 27 forward passes, sensible stage percentages.

**Cause:** the `#if` tested `__sparc__` and `__BYTE_ORDER__`, which are
**GCC** spellings. Sun Studio defines `__sparc` (one underscore) and
`__sparcv9`, defines no `__BYTE_ORDER__`, and `-Xc` suppresses the
unprefixed `sparc`. Every condition was false, so the `#else` picked
little-endian — on a big-endian machine. `1.0f` byte-reversed is
`4.6e-41`, so every quantisation scale became a denormal near zero.

**Fixes:** test every vendor spelling; make the `#else` an `#error`
rather than a guess; and call `inf_check_byte_order()` from
`gguf_open()` so a wrong `-D` on the command line is caught in one
second instead of eight minutes.

**Lesson:** a portability macro that guesses converts a build error into
a wrong-answer bug. If the code cannot tell, it must refuse.

---

## 2. `off_t` becomes a union you cannot touch

**Symptom, first attempt:**

```
"src/sys_posix.c", line 50: invalid cast expression
```

**Symptom after "fixing" it:** the same error one line later.

```
"src/sys_posix.c", line 59: argument #6 is incompatible with prototype:
    prototype: union {double *d, array[2] of int* l}
    argument : long
```

**Cause:** `-Xc` undefines `_LONGLONG_TYPE` because C90 has no
`long long`, so Solaris's `<sys/types.h>` falls back to
`typedef union { double _d; int32_t _l[2]; } longlong_t;`. With
`_FILE_OFFSET_BITS=64` on a **32-bit** build, `off_t` becomes that
union. Nothing can be cast to it, assigned to it, or passed to `mmap()`.

**Fix:** build 64-bit. In LP64 `off_t` is already a plain `long`,
large-file support is automatic, and the union never appears. The target
machines are all 64-bit anyway.

**Lesson:** when two "fixes" in a row move the error by one line, the
premise is wrong. Also: GCC accepted the original code silently as an
extension, which is why no Linux build ever caught it.

---

## 3. fp16 decoding silently returned zero on 64-bit big-endian

**Cause:**

```c
unsigned long bits;        /* 4 bytes on i486, 8 on LP64 */
...
memcpy(&out, &bits, 4);    /* copies the HIGH half on big-endian */
```

Every fp16 decoded to `0.0`, so every quantisation scale in the model
was zero.

**Lesson:** this is not an endianness bug, it is a **type-width**
assumption that only becomes visible when byte order changes. An audit
grepping for byte-swapping would never have found it — and did not: a
careful audit explicitly cleared this function as "pure integer bit
math, endianness-neutral".

---

## 4. MMX constants misaligned → fine on x86, fault on a Geode

**Cause:** constants declared `short[4]`, loaded with `movq`, which
requires 8-byte alignment. x86 silently splits a misaligned load; a
Geode faults.

A `union` with `long long` is **not** sufficient — the i386 SysV ABI
aligns `long long` to 4. The fix is explicit
`__attribute__((aligned(8)))`.

**Guard:** `tests/t_align.c` asserts it at run time, because the linked
layout is what matters, not the source.

**Note:** do not try to check this by grepping absolute addresses out of
the disassembly. 32-bit PIC builds reference constants
register-relative (`pand -0xaf04(%ebx)`), so there is nothing to grep.

---

## 5. Tool calls parsed "successfully" with no arguments

**Symptom:** `tool call: add {"a":17}` — the `b` silently missing. The
tool ran, returned a plausible number, and the model reported it
confidently.

**Cause:** Qwen3.5 emits tool calls in two shapes. Only the XML form was
handled. Given the JSON form, the parser did not *reject* it — it found
the name, found no `<parameter=` tags, and returned an empty argument
object.

**Lesson:** a parser that can partially match should say so rather than
return a partial result. And a tool-calling path is only tested when the
**arguments** are checked, not just the tool name.

---

## 6. Options parsed everywhere, read in one place

`--system`, `--think` and `--mcp` were parsed in every mode but only
*read* by `chat`. `infer run model.gguf --system "Answer in one word."`
produced a normal, plausible, wrong-length answer with no indication the
constraint had been dropped.

**Fix:** one option table that records which modes each flag applies to,
and rejects the combinations that do not. Restrictions then have to be
*justified* to be written down, which is why only five survived.

**Lesson:** silently ignored input is a correctness bug, not a UX one.

---

## 7. A profiling build that would not stop profiling

`infer-profile.exe` printed a full stage report after every request
whether asked or not, because `main.c` had
`prof_perf_enabled = log_perf || PROF_ENABLED`.

**Lesson:** building for measurement and asking for measurements are
different decisions. Compile the timers in; let a flag decide whether
anything is printed.

A related one: the "`--log-stages` needs a profile build" warning was
emitted *after* model loading, so with a wrong path the user never saw
it. Diagnostics about flags belong before any work starts.

---

## 8. Microbenchmarks lie about this workload, systematically

| optimisation | isolated | real hardware |
|---|---|---|
| nibble lookup tables | 2.46× | **0.97×** |
| multi-row kernel | modelled well | **0.90×** |
| bucket accumulation | 1.16× modelled | **0.76×** |
| product lookup table | promising | **0.52×** |

The dev host is out-of-order with a 3-cycle IMUL, 32 MB of L3 and a
large fast L1. The Geode is in-order with a ~16-cycle IMUL and a 4 KB
fast L1 window. Nothing transfers.

**Lesson:** measure end-to-end on the target, or do not claim a speedup.
See [`../docs/PERFORMANCE-ANALYSIS.md`](../docs/PERFORMANCE-ANALYSIS.md)
for the full record including every rejected approach.

---

## 9. A 1.34× "regression" that was Windows Update

Every stage in a profile was 1.26–1.37× slower — including pure-float
code that shares nothing with the changed kernels. Mean ratio 1.337,
standard deviation 0.036, and the *relative* profile unchanged to within
0.4 points.

A uniform multiplier across unrelated code is a **machine state**
signature, not a code change. It was a background Windows Update.

**Lesson:** before hunting a regression, check whether everything got
slower by the same factor. If it did, suspect the machine.

---

## 10. Sandbox hazards

* **Installed packages disappear between turns.** `command not found`
  mid-session means reinstall, not breakage.
* **Large workspaces get pruned**, sometimes taking source directories
  with them. Delete the 508 MB model when finished, and verify the tree
  is intact before packaging.
* **`make clean` only knew the current version's filenames**, so a stale
  binary from a previous version was swept into a release tarball.
  Fixed, but check what you are packaging.
* **Wine does not work** in these sandboxes. Verify Windows builds
  statically and test the Linux twin.
