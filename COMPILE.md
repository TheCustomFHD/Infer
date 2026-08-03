# Compiling infer

Every target, every flag, every verification step — as literal commands
you can paste.

The rule this document follows: **no step is described in prose if it
can be given as a command.** If you copy every block in a section in
order, starting from a fresh clone, you get a working binary and you
have proved it works.

---

## Contents

- [0. The 60-second version](#0-the-60-second-version)
- [1. What you need](#1-what-you-need)
- [2. Native build (Linux / macOS / BSD)](#2-native-build-linux--macos--bsd)
- [3. 32-bit i486](#3-32-bit-i486)
- [4. Linux + MMX (Geode LX, Pentium MMX, K6)](#4-linux--mmx-geode-lx-pentium-mmx-k6)
- [5. Windows XP](#5-windows-xp)
- [6. Solaris / SPARC](#6-solaris--sparc)
- [7. Other big-endian targets](#7-other-big-endian-targets)
- [8. Profiling builds](#8-profiling-builds)
- [9. Tests](#9-tests)
- [10. Every target, every flag](#10-every-target-every-flag)
- [11. Compile-time toggles](#11-compile-time-toggles)
- [12. Verifying a build you did not make](#12-verifying-a-build-you-did-not-make)
- [13. Troubleshooting](#13-troubleshooting)
- [14. Porting to a new platform](#14-porting-to-a-new-platform)

---

## 0. The 60-second version

```sh
git clone https://github.com/TheCustomFHD/Infer
cd Infer
make
```

That produces `infer-1.12.1`. Get a model and run it:

```sh
curl -L -o Qwen3.5-0.8B-Q4_K_M.gguf \
  "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf"

./infer-1.12.1 run Qwen3.5-0.8B-Q4_K_M.gguf -p "The capital of France is" -n 10 -t 0
```

Expected output:

```
The capital of France is **Paris**.
```

If you got that, everything below is optional.

### Why the version is in the filename

Every binary is named `infer-<version>[-variant]`, so several builds can
live in one directory without being renamed:

```sh
make version        # -> 1.12.1
make checkversion   # -> version 1.12.1 ok   (asserts Makefile == src/infer.h)
make SUFFIX=        # -> plain `infer`, no version, for scratch builds
```

---

## 1. What you need

The project has **no third-party dependencies**. Only libc, libm and the
OS socket API. There is nothing to `pip install`, no submodules, no
package manager step for the project itself — only for cross-compilers.

### Minimum

| | |
|---|---|
| a C89 compiler | GCC, Clang, Sun Studio, or any ANSI C compiler |
| `make` | GNU make or any POSIX make (Solaris `/usr/ccs/bin/make` works) |

### Per-target extras

**Debian / Ubuntu — everything at once:**

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    gcc-multilib \
    gcc-mingw-w64-i686-win32 \
    binutils-mingw-w64-i686
```

For the big-endian regression targets (optional, only if you want to
reproduce the byte-order testing):

```sh
sudo apt-get install -y \
    gcc-s390x-linux-gnu \
    gcc-sparc64-linux-gnu \
    qemu-user-static
```

**Fedora / RHEL:**

```sh
sudo dnf install -y gcc make glibc-devel.i686 libgcc.i686 \
                    mingw32-gcc binutils-mingw32
```

**Verify the toolchain before building:**

```sh
cc --version
make --version
i686-w64-mingw32-gcc --version    # only if you want Windows builds
```

### Two mingw gotchas

**Use the `-win32` variant, not `-posix`.** The posix-threads build links
`libwinpthread-1.dll`, which breaks the "three DLLs, all present on
stock XP" property. Check which you have:

```sh
i686-w64-mingw32-gcc -v 2>&1 | grep -o 'thread-model=[a-z]*'
```

Expected: `thread-model=win32`. If you get `posix`:

```sh
sudo apt-get install -y gcc-mingw-w64-i686-win32
sudo update-alternatives --config i686-w64-mingw32-gcc   # pick the win32 one
```

**Use `i686-`, not `x86_64-`.** The 64-bit mingw produces binaries XP
cannot load at all.

---

## 2. Native build (Linux / macOS / BSD)

```sh
make
```

Full command it runs:

```sh
cc -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter -O2 \
   -o infer-1.12.1 \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c \
   -lm
```

### Verify

```sh
./infer-1.12.1 --version
```
```
infer 1.12.1
```

```sh
./infer-1.12.1 --backend list
```
```
CPU: GenuineIntel  features: mmx cmov

available backends:
  i8    ok  integer kernels, portable C (recommended)   <- selected
  ref   ok  scalar float reference, maximum portability (486-safe)
```

The native target deliberately does **not** include MMX — see section 4.

### Zero warnings is the standard

```sh
make clean && make 2>&1 | grep -c 'warning:'
```
```
0
```

Any warning is a bug. Report it.

---

## 3. 32-bit i486

For genuinely old x86: 486, 586, early Pentium. No MMX, no CMOV, no SSE,
x87 for floating point.

```sh
make i486
```

Full command:

```sh
cc -std=c89 -pedantic -Wall -O2 \
   -m32 -march=i486 -mtune=i486 \
   -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 \
   -D_FILE_OFFSET_BITS=64 \
   -o infer-1.12.1-i486 \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c \
   -lm
```

### Verify it is really 486-safe

This is the check that matters. It disassembles the binary and counts
instructions the 486 does not have:

```sh
objdump -d infer-1.12.1-i486 --no-show-raw-insn \
  | grep -oE '^\s+[0-9a-f]+:\s+[a-z0-9.]+' \
  | awk '{print $2}' | sort -u \
  | grep -cE '^(cmov[a-z]*|pmaddwd|paddd|paddw|paddb|psubw|pand|por|pxor|punpck[a-z]+|psrlw|psllw|psraw|psrlq|movq|movd|emms|sfence|prefetch[a-z0-9]*|cvtt?s[sd]2[a-z]+|movap[sd]|movup[sd])$'
```
```
0
```

Anything other than `0` means the compiler emitted a post-486
instruction and the binary will fault on real hardware.

```sh
file infer-1.12.1-i486
```
```
ELF 32-bit LSB executable, Intel 80386, version 1 (SYSV), dynamically linked
```

---

## 4. Linux + MMX (Geode LX, Pentium MMX, K6)

```sh
make geode
```

Full command:

```sh
cc -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
   -m32 -march=i486 -mtune=geode \
   -mmmx -mno-sse -mno-sse2 -mfpmath=387 \
   -DINFER_HAVE_MMX -D_FILE_OFFSET_BITS=64 \
   -o infer-1.12.1-geode \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c \
   src/backend_mmx.c \
   -lm
```

### Read those flags carefully — they are deliberate

| flag | why |
|---|---|
| `-std=gnu89` | `backend_mmx.c` uses GNU inline asm. **This is the only file in the project that is not strict C89.** |
| `-march=i486` | **baseline**, not geode. Nothing newer than 486 is emitted for general code — no CMOV. |
| `-mtune=geode` | *scheduling* hints for Geode, which change instruction order but never the instruction set. |
| `-mmmx` | lets the compiler use MMX **in `backend_mmx.c`**, which is guarded by a runtime CPUID check. |
| `-DINFER_HAVE_MMX` | compiles the MMX backend in and registers it in the backend table. |

**The consequence: this one binary runs on a 486 *and* uses MMX on a
Geode.** The MMX kernels are selected at run time only if CPUID reports
MMX; otherwise the same binary transparently uses `i8`.

That is why `-march=i486` and not `-march=geode`: with `-march=geode`
GCC emits CMOV throughout ordinary code (`xmalloc`, `gguf_open`,
`main`), which faults on a 486 or original Pentium.

### Verify

```sh
# our code must contain no CMOV
objdump -d infer-1.12.1-geode --no-show-raw-insn \
  | grep -cE '^\s+[0-9a-f]+:\s+cmov[a-z]+'
```
```
0
```

```sh
# but the MMX kernels must be present
objdump -d infer-1.12.1-geode | grep -cE 'pmaddwd|punpcklbw|paddd'
```
```
354
```

```sh
# and MMX must be detected and chosen at run time
./infer-1.12.1-geode --backend list
```
```
CPU: GenuineIntel  features: mmx cmov

available backends:
  mmx   ok  integer kernels with MMX inner loop (Pentium MMX / K6 / Geode+)   <- selected
  i8    ok  integer kernels, portable C (recommended)
  ref   ok  scalar float reference, maximum portability (486-safe)
```

### Prove both backends agree

```sh
./infer-1.12.1-geode run model.gguf -p "The capital of France is" \
  -n 8 -t 0 --seed 1 --backend mmx -c 256

./infer-1.12.1-geode run model.gguf -p "The capital of France is" \
  -n 8 -t 0 --seed 1 --backend i8 -c 256
```

`-t 0` is greedy, so both must print exactly the same text. If they
differ, one kernel is wrong.

### The MMX alignment trap

`backend_mmx.c` loads constants with `movq`, which **requires 8-byte
alignment**. x86 silently splits a misaligned load; a Geode faults.
`tests/t_align.c` asserts it, and you can check the linked binary
directly:

The reliable check is the run-time one, because it inspects the actual
addresses the loader produced:

```sh
make test
./build/t_align
```
```
MMX constant alignment (movq requires % 8 == 0):
  c_mask_f   0x55d7ea6a5798  % 8 = 0  ok
  c_mask_1   0x55d7ea6a5788  % 8 = 0  ok
  c_mask_3   0x55d7ea6a5778  % 8 = 0  ok
  c_8        0x55d7ea6a5770  % 8 = 0  ok
  c_32       0x55d7ea6a5768  % 8 = 0  ok

all MMX constants correctly aligned
```

Run this **on the target machine** if you can — it is the only check
that accounts for how that loader lays the binary out.

Statically, confirm `.rodata` is aligned at least 8:

```sh
readelf -S infer-1.12.1-geode | grep -A1 '\.rodata'
```
```
  [15] .rodata  PROGBITS  0001d000 01d000 006118 00   A  0   0 32
```

The last column is the alignment — `32` here, comfortably above 8.

(Do not try to grep absolute addresses out of the disassembly: 32-bit
PIC builds reference constants register-relative, e.g. `pand
-0xaf04(%ebx)`, so there is no absolute address to check.)

---

## 5. Windows XP

Two binaries, both 32-bit, both XP-compatible.

### 5a. Plain (no MMX)

```sh
make windows
```

```sh
i686-w64-mingw32-gcc -std=c89 -pedantic -Wall -O2 \
   -march=i486 -mtune=i486 \
   -D_WIN32_WINNT=0x0501 \
   -o infer-1.12.1.exe \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_win32.c src/sys_win32.c \
   -lws2_32
```

### 5b. With MMX — this is the one to ship

```sh
make windows-mmx
```

```sh
i686-w64-mingw32-gcc -std=gnu89 -Wall -O2 \
   -march=i486 -mtune=geode -mmmx \
   -DINFER_HAVE_MMX -D_WIN32_WINNT=0x0501 \
   -o infer-1.12.1-mmx.exe \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_win32.c src/sys_win32.c \
   src/backend_mmx.c \
   -lws2_32
```

Same reasoning as section 4: `-march=i486` baseline plus runtime-gated
MMX, so `infer-1.12.1-mmx.exe` runs on a 486-class machine *and*
accelerates on anything with MMX. **`infer-1.12.1-mmx.exe` supersedes
`infer-1.12.1.exe` for almost every use.**

### `-D_WIN32_WINNT=0x0501`

Pins the Windows API surface to XP. The compiler then refuses anything
newer, which is what keeps the import table clean.

### Verify

```sh
i686-w64-mingw32-objdump -p infer-1.12.1-mmx.exe | grep 'DLL Name'
```
```
	DLL Name: KERNEL32.dll
	DLL Name: msvcrt.dll
	DLL Name: WS2_32.dll
```

Exactly three, all present on a stock XP install. Count them:

```sh
i686-w64-mingw32-objdump -p infer-1.12.1-mmx.exe | grep -c 'DLL Name'
```
```
3
```

Check for post-XP APIs that would fail to resolve at load time:

```sh
i686-w64-mingw32-objdump -p infer-1.12.1-mmx.exe \
  | grep -oE '\b(GetTickCount64|InitializeCriticalSectionEx|CreateFile2|GetFinalPathNameByHandle|InetPton|GetAddrInfoW|SetThreadStackGuarantee)\b' \
  | sort -u
```

Must print nothing.

```sh
file infer-1.12.1-mmx.exe
```
```
PE32 executable (console) Intel 80386, for MS Windows
```

### An honest caveat about the CRT

Our code contains no CMOV. **mingw's C runtime does** — in `strtod`,
`pow` and `ldexp`, which we do call (`pow` during RoPE setup, `strtod`
in the JSON and Jinja parsers).

```sh
i686-w64-mingw32-objdump -d infer-1.12.1-mmx.exe --no-show-raw-insn \
  | grep -cE '^\s+[0-9a-f]+:\s+cmov[a-z]+'
```
```
36
```

All 36 are in mingw CRT functions, none in ours. So the Windows builds
are **Pentium-Pro-and-later** in practice, even though our own code is
486-clean. For a genuine 486 running Windows, use the Linux i486 target
or build with a CRT that predates CMOV. This has not been tested on a
real 486 and is stated as a limitation, not a claim.

---

## 6. Solaris / SPARC

Tested against **Sun Studio 12.3** on UltraSPARC.

```sh
make solaris              # Sun Studio (cc)
make solaris-gcc          # GCC on Solaris
make solaris-profile      # Sun Studio + stage timers
make solaris-gcc-profile  # GCC + stage timers
make solaris-test         # the test programs
```

`make solaris` runs:

```sh
cc -Xc -xc99=none -xO3 -xtarget=native \
   -DINFER_BIG_ENDIAN=1 -D_FILE_OFFSET_BITS=64 \
   -o infer-1.12.1 \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c \
   -lm -lsocket -lnsl
```

### The three things that are easy to get wrong

**`-lsocket -lnsl`** — Solaris keeps the socket API outside libc. Without
these you get undefined `socket`, `bind`, `gethostbyname` at link time.
This is the most likely reason a hand-rolled GCC build fails on Solaris
where Sun Studio succeeds.

**Sun Studio option spellings** — `-Xc -xc99=none` is strict C90 (GCC's
`-std=c89 -pedantic`), `-xO3` is `-O2`-ish, `-xtarget=` is `-march=`.
GCC's spellings are not recognised by `cc` at all.

**`-DINFER_BIG_ENDIAN=1`** — explicit on purpose. Sun Studio does not
define `__BYTE_ORDER__`, and `-Xc` suppresses the unprefixed `sparc`
macro. Autodetection therefore has less to work with than under GCC.
Since 1.12.1 the build *fails* rather than guessing wrong, but being
explicit documents the intent. See finding 20 in
[docs/FINDINGS.md](docs/FINDINGS.md) for what happened when it guessed.

### Optimisation flags

The defaults are `-xO5 -xunroll=16`, adopted from field use on an
UltraSPARC. Override with `SUNOPT`:

```sh
make solaris SUNOPT="-xO3"              # more conservative
make solaris SUNOPT="-xO5 -xunroll=8"
```

Two Sun Studio flags you may see recommended are **not** enabled by
default:

| flag | why not |
|---|---|
| `-fast` | A macro enabling `-fsimple=2` and `-fns`: non-IEEE float, reassociation, flush-to-zero. The k-quant scales are fp16 values near zero and the DeltaNet recurrence accumulates over 128 steps, so flush-to-zero is a genuine correctness risk here. |
| `-xvis` | Enables VIS intrinsics. There is no VIS code in this project, so it does nothing. Harmless. |

To try `-fast` anyway:

```sh
make solaris-fast
./build/t_backend model.gguf     # check rel-l2 against the ref column
```

`rel-l2` should stay around `3.6e-03` for i8. If it grows, `-fast` is
changing your results.

### Cross-building on a fast machine for a slow one

```sh
make solaris SUNTARGET=ultra2      # build on a Blade 2500 for an Ultra 10
```

`SUNTARGET` defaults to `native`. Valid values include `ultra`,
`ultra2`, `ultra3`, `ultraT1`, `generic`.

### Verify

```sh
./infer-1.12.1 --backend list
```
```
CPU: unknown  features:

available backends:
  i8    ok  integer kernels, portable C (recommended)   <- selected
  ref   ok  scalar float reference, maximum portability (486-safe)
```

`CPU: unknown` is correct — CPUID is x86-only. The `i8` backend is
portable C and is **6.5–8.8× faster than `ref`** on SPARC (measured per
weight format). There is no VIS backend.

Then prove the byte order is right:

```sh
make solaris-test
./build/t_endian model.gguf
```

The tensor values it prints must match a run on any other machine
byte-for-byte.

---

## 7. Other big-endian targets

Used for byte-order regression testing. Both need
`qemu-user-static` to run the results on an x86 host.

### s390x (IBM z/Architecture)

```sh
sudo apt-get install -y gcc-s390x-linux-gnu qemu-user-static

s390x-linux-gnu-gcc -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter \
   -O2 -static -o infer-1.12.1-s390x-linux \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c -lm

qemu-s390x-static ./infer-1.12.1-s390x-linux run model.gguf \
    -p "The capital of France is" -n 8 -t 0 --seed 1 -c 256
```

### SPARC64 (Linux, **not** Solaris)

```sh
sudo apt-get install -y gcc-sparc64-linux-gnu qemu-user-static

sparc64-linux-gnu-gcc -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter \
   -O2 -static -D_FILE_OFFSET_BITS=64 -o infer-1.12.1-sparc64-linux \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c -lm

qemu-sparc64-static ./infer-1.12.1-sparc64-linux run model.gguf \
    -p "The capital of France is" -n 8 -t 0 --seed 1 -c 256
```

**These are Linux binaries. They will not run on Solaris** — different
ABI, different libc. For Solaris use section 6.

### The byte-order test without a model

```sh
s390x-linux-gnu-gcc -std=c89 -O2 -static -o t_endian_be \
   tests/t_endian.c src/gguf.c src/quant.c src/backend.c \
   src/util.c src/prof.c src/sys_posix.c -lm

qemu-s390x-static ./t_endian_be
```
```
host is big-endian (INFER_BIG_ENDIAN=1)

inf_check_byte_order() passes on this host           ok
INFER_BIG_ENDIAN agrees with a runtime probe         ok
inf_rd_f32p(00 00 80 3F) == 1.0                      ok
...
all endian tests passed
```

---

## 8. Profiling builds

Identical to the normal targets plus `-DINFER_PROFILE`, which compiles
in per-stage timers.

```sh
make profile             # native
make profile-i486        # 32-bit i486
make profile-geode       # 32-bit + MMX
make profile-windows     # Windows + MMX
make solaris-profile     # Solaris, Sun Studio
```

Outputs are `infer-1.12.1-profile`, `-i486-profile`, `-geode-profile`,
and `infer-1.12.1-profile.exe`.

### Both logging features are opt-in

A profiling build prints **nothing** until asked. This matters: before
1.10.0 it dumped a report after every request whether you wanted one or
not.

```sh
./infer-1.12.1-profile run model.gguf -p "hi" -n 10 \
    --log-perf                          # throughput only
./infer-1.12.1-profile run model.gguf -p "hi" -n 10 \
    --log-perf --log-stages             # + the per-stage table
./infer-1.12.1-profile run model.gguf -p "hi" -n 10 \
    --log-perf --log-stages --log-file perf.txt   # to a file
```

`--log-stages` on a non-profiling build tells you so rather than
silently doing nothing:

```
--log-stages needs a build with -DINFER_PROFILE (make profile /
profile-geode / profile-windows); this build has no stage timers
compiled in
```

Without `-DINFER_PROFILE` every timer macro expands to nothing — not a
branch, not an instruction. Verify:

```sh
nm infer-1.12.1 | grep -c prof_add        # 0
nm infer-1.12.1-profile | grep -c prof_add # 1
```

Measured overhead of the profiling build: **~2%**.

See [PROFILING.md](PROFILING.md) for how to read the output.

---

## 9. Tests

```sh
make test
```

Builds eight programs into `build/`:

```sh
./build/t_align                        # no model needed
./build/t_agent                        # no model needed
./build/t_endian     [model.gguf]      # model optional
./build/t_jinja       model.gguf
./build/t_quant       model.gguf
./build/t_tokenizer   model.gguf
./build/t_engine      model.gguf
./build/t_backend     model.gguf       # also a benchmark
```

Expected final lines:

```
all MMX constants correctly aligned
all passed
all endian tests passed
all jinja tests passed
all tokenizer tests passed
```

### Run everything in one go

```sh
make test && \
./build/t_align && \
./build/t_agent && \
./build/t_endian && \
./build/t_jinja model.gguf && \
./build/t_tokenizer model.gguf && \
echo "ALL TESTS PASSED"
```

### What each one proves

| test | needs a model | proves |
|---|---|---|
| `t_align` | no | MMX constants are 8-byte aligned — guards a real fault |
| `t_agent` | no | chat/run/serve render identical prompts; every tool-call shape parses |
| `t_endian` | optional | byte order is right; same numbers on any host |
| `t_jinja` | optional | 25 expression cases + the real chat template |
| `t_quant` | yes | dequantised values match a reference decoder |
| `t_tokenizer` | yes | 14 round-trips: CJK, emoji, contractions, whitespace |
| `t_engine` | yes | end-to-end greedy generation |
| `t_backend` | yes | every backend vs `ref`, accuracy **and** throughput |

`make bench` builds only `t_backend`.

---

## 10. Every target, every flag

| target | output | key flags |
|---|---|---|
| `make` | `infer-1.12.1` | `-std=c89 -pedantic -Wall -Wextra -O2` |
| `make i486` | `infer-1.12.1-i486` | `-m32 -march=i486 -mno-sse -mno-mmx -mfpmath=387 -D_FILE_OFFSET_BITS=64` |
| `make geode` | `infer-1.12.1-geode` | `-std=gnu89 -m32 -march=i486 -mtune=geode -mmmx -DINFER_HAVE_MMX` |
| `make windows` | `infer-1.12.1.exe` | `-march=i486 -D_WIN32_WINNT=0x0501 -lws2_32` |
| `make windows-mmx` | `infer-1.12.1-mmx.exe` | `+ -mtune=geode -mmmx -DINFER_HAVE_MMX` |
| `make solaris` | `infer-1.12.1` | `-Xc -xc99=none -xO3 -xtarget=native -DINFER_BIG_ENDIAN=1 -lsocket -lnsl` |
| `make solaris-gcc` | `infer-1.12.1` | GCC equivalent `+ -lsocket -lnsl` |
| `make profile` | `infer-1.12.1-profile` | `+ -DINFER_PROFILE` |
| `make profile-i486` | `infer-1.12.1-i486-profile` | i486 `+ -DINFER_PROFILE` |
| `make profile-geode` | `infer-1.12.1-geode-profile` | geode `+ -DINFER_PROFILE` |
| `make profile-windows` | `infer-1.12.1-profile.exe` | windows-mmx `+ -DINFER_PROFILE` |
| `make solaris-profile` | `infer-1.12.1-profile` | solaris `+ -DINFER_PROFILE` |
| `make test` | `build/t_*` | eight test programs |
| `make bench` | `build/t_backend` | benchmark only |
| `make version` | — | prints `1.12.1` |
| `make checkversion` | — | asserts Makefile matches `src/infer.h` |
| `make clean` | — | removes all of the above |

### The two shipped builds

Since 1.12.1 these are the binaries produced for every release:

```sh
make windows-mmx      # infer-1.12.1-mmx.exe       Windows XP, MMX + 486 fallback
make profile-windows  # infer-1.12.1-profile.exe   ...with stage timers
make geode            # infer-1.12.1-geode         Linux, MMX + 486 fallback
make profile-geode    # infer-1.12.1-geode-profile ...with stage timers
```

The MMX builds have a 486 baseline, so a separate non-MMX build is
redundant: one binary covers both cases via runtime detection.

---

## 11. Compile-time toggles

| define | effect |
|---|---|
| `-DINFER_HAVE_MMX` | compiles `backend_mmx.c` in and registers the `mmx` backend. Requires `-mmmx` and `-std=gnu89`. |
| `-DINFER_PROFILE` | compiles in per-stage timers. Without it, every timer macro expands to nothing. |
| `-DINFER_BIG_ENDIAN=1` / `=0` | forces byte order. Autodetected; the build **fails** rather than guessing if it cannot tell. |
| `-DINFER_BACKEND="i8"` | changes the default backend when none is given on the command line. |
| `-DINFER_VERSION="..."` | overrides the version string. |
| `-D_FILE_OFFSET_BITS=64` | 64-bit `off_t` on 32-bit hosts. Needed for models >2 GB. |
| `-D_WIN32_WINNT=0x0501` | pins the Windows API to XP. |

Example — a 486 build that defaults to `ref`:

```sh
cc -std=c89 -pedantic -O2 -m32 -march=i486 -mno-mmx -mfpmath=387 \
   -DINFER_BACKEND='"ref"' -D_FILE_OFFSET_BITS=64 \
   -o infer-ref src/*.c -lm     # note: excludes backend_mmx.c
```

`src/*.c` works only when `backend_mmx.c` is absent or you also pass
`-DINFER_HAVE_MMX -mmmx -std=gnu89`. The Makefile lists sources
explicitly for that reason.

---

## 12. Verifying a build you did not make

Everything below runs on the binary, not the source.

```sh
# 1. it is the architecture you expect
file infer-1.12.1-geode

# 2. version
./infer-1.12.1-geode --version

# 3. which kernels are compiled in, and which the CPU can use
./infer-1.12.1-geode --backend list

# 4. no post-486 instructions (i486 target only)
objdump -d infer-1.12.1-i486 --no-show-raw-insn \
  | grep -oE '^\s+[0-9a-f]+:\s+[a-z0-9.]+' | awk '{print $2}' | sort -u \
  | grep -cE '^(cmov[a-z]*|pmaddwd|paddd|punpck[a-z]+|movq|emms)$'      # want 0

# 5. no CMOV in our code (geode target)
objdump -d infer-1.12.1-geode --no-show-raw-insn \
  | grep -cE '^\s+[0-9a-f]+:\s+cmov[a-z]+'                              # want 0

# 6. MMX constants 8-byte aligned (run on the target machine)
make test && ./build/t_align                    # "all MMX constants correctly aligned"
readelf -S infer-1.12.1-geode | grep -A1 '\.rodata'   # last column >= 8

# 7. Windows: three DLLs
i686-w64-mingw32-objdump -p infer-1.12.1-mmx.exe | grep -c 'DLL Name'   # want 3

# 8. both backends agree bit-for-bit (greedy decoding)
./infer-1.12.1-geode run model.gguf -p "test" -n 20 -t 0 --seed 1 --backend mmx
./infer-1.12.1-geode run model.gguf -p "test" -n 20 -t 0 --seed 1 --backend i8
```

---

## 13. Troubleshooting

**`fatal error: bits/libc-header-start.h: No such file or directory`**

32-bit headers missing:

```sh
sudo apt-get install -y gcc-multilib libc6-dev-i386
```

**`i686-w64-mingw32-gcc: command not found`**

```sh
sudo apt-get install -y gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686
```

**The .exe imports four DLLs, one is `libwinpthread-1.dll`**

You used the posix-threads mingw. Install the `-win32` variant:

```sh
sudo apt-get install -y gcc-mingw-w64-i686-win32
sudo update-alternatives --config i686-w64-mingw32-gcc
```

**Undefined `socket`, `bind`, `gethostbyname` on Solaris**

Missing `-lsocket -lnsl`. Use `make solaris` or `make solaris-gcc`,
which pass them.

**`#error "cannot determine byte order"`**

Your compiler is not recognised. Say which it is:

```sh
make CFLAGS="-std=c89 -O2 -DINFER_BIG_ENDIAN=1"    # big-endian
make CFLAGS="-std=c89 -O2 -DINFER_BIG_ENDIAN=0"    # little-endian
```

This deliberately fails rather than guessing — a wrong guess produces a
program that loads the model, prints correct metadata, runs at full
speed and emits garbage.

**`byte order mismatch` at startup**

The binary was compiled for the wrong endianness. The message tells you
what to rebuild with. See finding 20.

**MMX works on a modern desktop, crashes on real old hardware**

Almost certainly the constant alignment issue. Run `./build/t_align` on
the target machine and check step 6 in section 12.

**Output is garbage / `!!!!!!!!`**

```sh
./build/t_endian model.gguf        # byte order
./build/t_backend model.gguf       # kernel accuracy vs ref
./infer-... run model.gguf --backend ref -p "test" -t 0    # is ref right?
```

If `ref` is correct and `i8`/`mmx` are not, it is a kernel bug. If `ref`
is also wrong, suspect byte order first.

**`make: *** No rule to make target` on Solaris**

You are using a make that does not understand something in the file.
The Makefile is POSIX-clean as of 1.12.1 — if this happens, report it.
Workaround:

```sh
gmake solaris
```

---

## 14. Porting to a new platform

Only two files know an operating system exists:

```
src/net.h   ->  src/net_posix.c   or  src/net_win32.c
src/sys.h   ->  src/sys_posix.c   or  src/sys_win32.c
```

To port:

1. Write `src/net_<plat>.c` implementing the 12 functions in `net.h`.
2. Write `src/sys_<plat>.c` implementing file mapping and a monotonic
   clock (`sys.h`). Both have documented fallbacks — `sys_map_file()`
   may return NULL and the caller reads the file into RAM instead.
3. Add a Makefile target.
4. Nothing else changes. The engine, kernels, tokeniser, template
   engine and server are all platform-independent C89.

Then verify in this order:

```sh
./build/t_endian          # byte order first — everything depends on it
./build/t_align           # if you compiled an MMX backend
./build/t_quant  model.gguf
./build/t_backend model.gguf
./build/t_engine model.gguf
```

`t_endian` first is not arbitrary: a byte-order mistake makes every
later test fail in confusing ways.

---

## Measured build times

Single-threaded, on a modern x86-64 laptop:

| target | time |
|---|---|
| `make` | ~3 s |
| `make i486` | ~3 s |
| `make geode` | ~4 s |
| `make windows-mmx` | ~5 s |
| `make test` | ~10 s |
| all targets | ~45 s |

There are no object files and no incremental build: every target
compiles all sources in one command. At this size that is faster than
managing dependencies, and it makes each target a single copy-pasteable
line.

---

## See also

- [README.md](README.md) — what this is
- [USER-GUIDE.md](USER-GUIDE.md) — every command and flag
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the pieces fit
- [docs/FINDINGS.md](docs/FINDINGS.md) — the surprises, with measurements
- [PROFILING.md](PROFILING.md) — reading the performance output
