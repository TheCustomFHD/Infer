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
- [2. **For Arena AI agents: the exact working recipe**](#2-for-arena-ai-agents-the-exact-working-recipe)
- [3. Linux](#3-linux)
- [4. Windows XP](#4-windows-xp)
- [5. Profiling builds](#5-profiling-builds)
- [6. Solaris / SPARC](#6-solaris--sparc)
- [7. Other big-endian targets](#7-other-big-endian-targets)
- [8. Tests](#8-tests)
- [9. Every target, every flag](#9-every-target-every-flag)
- [10. Compile-time toggles](#10-compile-time-toggles)
- [11. Verifying a build you did not make](#11-verifying-a-build-you-did-not-make)
- [12. Troubleshooting](#12-troubleshooting)
- [13. Porting to a new platform](#13-porting-to-a-new-platform)

---

## 0. The 60-second version

```sh
git clone https://github.com/TheCustomFHD/Infer
cd Infer
make linux
```

That produces `infer-1.14.0-linux`. Get a model and run it:

```sh
curl -L -o Qwen3.5-0.8B-Q4_K_M.gguf \
  "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf"

./infer-1.14.0-linux run Qwen3.5-0.8B-Q4_K_M.gguf \
    -p "The capital of France is" -n 10 -t 0
```

Expected output:

```
The capital of France is **Paris**.
```

If you got that, everything below is optional.

### There are only four targets

```sh
make linux              # -> infer-1.14.0-linux
make linux-profile      # -> infer-1.14.0-linux-profile
make windows            # -> infer-1.14.0-windows.exe
make windows-profile    # -> infer-1.14.0-windows-profile.exe

make all                # all four
make help               # this list
```

**Every one of those contains all three backends** — `mmx`, `i8`, `ref` —
and picks the fastest the CPU supports at run time. **Every one is built
to an i486 baseline**, so the same binary runs on a 486 and accelerates
on anything with MMX.

There is no separate "MMX build" and "non-MMX build", and no separate
"i486 build". One binary covers all of it. The only axis is whether you
want the profiler.

Solaris/SPARC is a separate world with different flags — section 6.

### Why the version is in the filename

Every binary is named `infer-<version>-<platform>`, so several builds
can live in one directory without being renamed:

```sh
make version        # -> 1.14.0
make checkversion   # -> version 1.14.0 ok   (asserts Makefile == src/infer.h)
make SUFFIX=        # -> infer-linux, no version, for scratch builds
```

---

## 1. What you need

The project has **no third-party dependencies**. Only libc, libm and the
OS socket API. There is nothing to `pip install`, no submodules, no
vendored libraries.

### Minimum

| | |
|---|---|
| a C89 compiler | GCC, Clang, Sun Studio, or any ANSI C compiler |
| `make` | GNU make or any POSIX make (Solaris `/usr/ccs/bin/make` works) |

### Debian / Ubuntu — everything at once

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    gcc-multilib \
    gcc-mingw-w64-i686-win32 \
    binutils-mingw-w64-i686
```

`gcc-multilib` is needed for `make linux` (it is a 32-bit build).
The two mingw packages are needed for `make windows`.

### Fedora / RHEL

```sh
sudo dnf install -y gcc make glibc-devel.i686 libgcc.i686 \
                    mingw32-gcc binutils-mingw32
```

### Verify the toolchain before building

```sh
cc --version
make --version
i686-w64-mingw32-gcc --version
```

### Two mingw gotchas that cost people hours

**Use the `-win32` variant, not `-posix`.** The posix-threads build
links `libwinpthread-1.dll`, which breaks the "three DLLs, all present
on stock XP" property. Check:

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

## 2. For Arena AI agents: the exact working recipe

Several agents have failed to produce a Windows XP build. This is the
literal sequence that works, verified in a clean Debian sandbox. Run it
top to bottom; do not substitute steps.

### Step 1 — install the toolchain

```sh
sudo -n apt-get update -qq
sudo -n apt-get install -y -qq \
    build-essential gcc-multilib \
    gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686
```

**This must be re-run in every session.** Sandboxes discard installed
packages between turns. If `make windows` suddenly fails with
`i686-w64-mingw32-gcc: command not found`, this is why — reinstall and
continue.

### Step 2 — confirm the right compiler variant

```sh
i686-w64-mingw32-gcc --version | head -1
i686-w64-mingw32-gcc -v 2>&1 | grep -o 'thread-model=[a-z]*'
```

Expected:

```
i686-w64-mingw32-gcc (GCC) 14-win32
thread-model=win32
```

If `thread-model=posix`, stop and fix it — see section 1. Everything
downstream will silently produce a binary with a fourth DLL dependency.

### Step 3 — build

```sh
make windows
make windows-profile
```

That is the whole build. No configure, no cmake, no object files.

### Step 4 — verify, because "it compiled" is not "it works on XP"

```sh
i686-w64-mingw32-objdump -p infer-1.14.0-windows.exe | grep -c 'DLL Name'
```
```
3
```

Anything other than `3` means it will not run on a stock XP install.
See which they are:

```sh
i686-w64-mingw32-objdump -p infer-1.14.0-windows.exe | grep 'DLL Name'
```
```
	DLL Name: KERNEL32.dll
	DLL Name: msvcrt.dll
	DLL Name: WS2_32.dll
```

Confirm it is a 32-bit PE:

```sh
file infer-1.14.0-windows.exe
```
```
PE32 executable (console) Intel 80386, for MS Windows
```

Confirm the MMX kernels are in there:

```sh
i686-w64-mingw32-objdump -d infer-1.14.0-windows.exe \
  | grep -cE 'pmaddwd|punpcklbw'
```
```
206
```

Confirm no post-XP API crept in:

```sh
i686-w64-mingw32-objdump -p infer-1.14.0-windows.exe \
  | grep -oE '\b(GetTickCount64|InitializeCriticalSectionEx|CreateFile2|InetPton|GetAddrInfoW|SetThreadStackGuarantee)\b' \
  | sort -u
```

Must print nothing.

### Step 5 — you cannot run it here, so test the Linux twin

Wine is unreliable in most sandboxes (Debian's Wine 10 fails its own
prefix bootstrap with `user32.dll.CreateDialogParamW` unimplemented).
Do not burn time on it.

`make linux` builds the *same sources with the same flags* and only
swaps `net_win32.c`/`sys_win32.c` for their POSIX equivalents. If the
Linux build is correct, the Windows one almost certainly is:

```sh
make linux

curl -L -o model.gguf \
  "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf"

./infer-1.14.0-linux run model.gguf -p "The capital of France is" \
    -n 8 -t 0 --seed 1 --backend mmx -c 256
./infer-1.14.0-linux run model.gguf -p "The capital of France is" \
    -n 8 -t 0 --seed 1 --backend i8 -c 256
```

`-t 0` is greedy, so both **must** print identical text. If they differ,
one kernel is broken and the Windows build has the same bug.

### Step 6 — clean up before you finish

The model is 508 MB and sandboxes prune large workspaces without
warning, sometimes taking your source with them:

```sh
rm -f model.gguf
make clean
```

### Common agent mistakes, in the order they happen

| symptom | cause |
|---|---|
| `i686-w64-mingw32-gcc: command not found` | packages were discarded between turns; reinstall |
| four DLLs, one is `libwinpthread-1.dll` | posix-threads mingw; install the `-win32` variant |
| `bits/libc-header-start.h: No such file` | `gcc-multilib` missing, needed for the 32-bit `make linux` |
| binary is `PE32+` | used `x86_64-w64-mingw32-gcc`; use `i686-` |
| `cannot open 'model.gguf'` mid-session | the sandbox pruned it; re-download |
| tried to run the `.exe` under Wine | do not; verify statically and test the Linux twin |
| `--log-stages` prints nothing | you built the non-profile target; use `make windows-profile` |

---

## 3. Linux

```sh
make linux
```

Full command it runs:

```sh
cc -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
   -march=i486 -mtune=geode -mmmx -mno-sse -mno-sse2 -mfpmath=387 \
   -DINFER_HAVE_MMX \
   -m32 -D_FILE_OFFSET_BITS=64 \
   -o infer-1.14.0-linux \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c \
   src/backend_mmx.c \
   -lm
```

### Read those flags carefully — every one is deliberate

| flag | why |
|---|---|
| `-std=gnu89` | `backend_mmx.c` uses GNU inline asm. **The only file in the project that is not strict C89.** |
| `-march=i486` | **Baseline.** Nothing newer than a 486 is emitted for ordinary code — in particular no CMOV, which is a Pentium Pro instruction. |
| `-mtune=geode` | **Scheduling only.** Reorders for the Geode pipeline; never changes the instruction set. |
| `-mmmx` | Lets the compiler use MMX *inside `backend_mmx.c`*, which is guarded by a runtime CPUID check. |
| `-DINFER_HAVE_MMX` | Compiles that backend in and registers it in the backend table. |
| `-mfpmath=387` | x87 floating point, not SSE. |
| `-D_FILE_OFFSET_BITS=64` | 64-bit `off_t` on a 32-bit host, for models over 2 GB. |

**The consequence: one binary, all three backends, runs on a 486, uses
MMX on a Geode.** The kernels are selected at run time from CPUID.

Why `-march=i486` and not `-march=geode`: with `-march=geode` GCC emits
CMOV throughout ordinary code — `xmalloc`, `gguf_open`, `main` — which
faults on a 486 or original Pentium. Measured cost of the i486 baseline:
**0.6%, in favour of the baseline**. Of the 30 CMOVs a `-march=geode`
build emits, only 3 are in a hot function.

### Verify

```sh
./infer-1.14.0-linux --backend list
```
```
CPU: GenuineIntel  features: mmx cmov

available backends:
  mmx   ok  integer kernels with MMX inner loop (Pentium MMX / K6 / Geode+)   <- selected
  i8    ok  integer kernels, portable C (recommended)
  ref   ok  scalar float reference, maximum portability (486-safe)
```

All three present. Now the 486 guarantee:

```sh
objdump -d infer-1.14.0-linux --no-show-raw-insn \
  | grep -cE '^\s+[0-9a-f]+:\s+cmov[a-z]+'
```
```
0
```

And the MMX kernels are really there:

```sh
objdump -d infer-1.14.0-linux | grep -cE 'pmaddwd|punpcklbw|paddd'
```
```
354
```

### Prove the backends agree

```sh
./infer-1.14.0-linux run model.gguf -p "The capital of France is" \
    -n 8 -t 0 --seed 1 --backend mmx -c 256
./infer-1.14.0-linux run model.gguf -p "The capital of France is" \
    -n 8 -t 0 --seed 1 --backend i8 -c 256
./infer-1.14.0-linux run model.gguf -p "The capital of France is" \
    -n 8 -t 0 --seed 1 --backend ref -c 256
```

`-t 0` is greedy: all three must print exactly the same text.

### The MMX alignment trap

`backend_mmx.c` loads constants with `movq`, which **requires 8-byte
alignment**. x86 silently splits a misaligned load; a Geode faults.
Declaring the constants `short[4]` gives 4-byte alignment, and a `union`
with `long long` is **not** sufficient — the i386 SysV ABI aligns
`long long` to 4. The fix is an explicit `__attribute__((aligned(8)))`.

The reliable check is the run-time one, because it inspects the
addresses the loader actually produced:

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

Run it **on the target machine** if you can — it is the only check that
accounts for that loader's layout.

Statically, confirm `.rodata` is aligned at least 8:

```sh
readelf -S infer-1.14.0-linux | grep -A1 '\.rodata'
```

The last column is the alignment. Do not try to grep absolute addresses
out of the disassembly: 32-bit PIC builds reference constants
register-relative (`pand -0xaf04(%ebx)`), so there is no absolute
address to check.

---

## 4. Windows XP

```sh
make windows
```

Full command:

```sh
i686-w64-mingw32-gcc -std=gnu89 -Wall -Wextra -Wno-unused-parameter -O2 \
   -march=i486 -mtune=geode -mmmx -mno-sse -mno-sse2 -mfpmath=387 \
   -DINFER_HAVE_MMX \
   -D_WIN32_WINNT=0x0501 \
   -o infer-1.14.0-windows.exe \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_win32.c src/sys_win32.c \
   src/backend_mmx.c \
   -lws2_32
```

Identical flags to the Linux target except for the two platform files
and `-D_WIN32_WINNT=0x0501`, which pins the Windows API surface to XP so
the compiler refuses anything newer.

### Verify

```sh
i686-w64-mingw32-objdump -p infer-1.14.0-windows.exe | grep 'DLL Name'
```
```
	DLL Name: KERNEL32.dll
	DLL Name: msvcrt.dll
	DLL Name: WS2_32.dll
```

All three are present on a stock XP install.

```sh
file infer-1.14.0-windows.exe
```
```
PE32 executable (console) Intel 80386, for MS Windows
```

### An honest caveat about the CRT

Our code contains **no CMOV**. mingw's C runtime does — in `strtod`,
`pow` and `ldexp`, all of which we call (`pow` during RoPE setup,
`strtod` in the JSON and Jinja parsers):

```sh
i686-w64-mingw32-objdump -d infer-1.14.0-windows.exe --no-show-raw-insn \
  | grep -cE '^\s+[0-9a-f]+:\s+cmov[a-z]+'
```
```
36
```

All 36 are in mingw CRT functions, none in ours. So the Windows builds
are **Pentium-Pro-and-later in practice**, even though our own code is
486-clean. For a genuine 486, use the Linux target, which is verified at
0 CMOV. This has not been tested on real 486 hardware and is stated as a
limitation, not a claim.

---

## 5. Profiling builds

```sh
make linux-profile
make windows-profile
```

Identical to the normal targets plus `-DINFER_PROFILE`. Overhead is
about **2%**.

### Both logging features are opt-in

A profiling build prints **nothing** until asked:

```sh
./infer-1.14.0-linux-profile run model.gguf -p "hi" -n 10 \
    --log-perf                                    # throughput only
./infer-1.14.0-linux-profile run model.gguf -p "hi" -n 10 \
    --log-perf --log-stages                       # + per-stage table
./infer-1.14.0-linux-profile run model.gguf -p "hi" -n 10 \
    --log-perf --log-stages --log-file perf.txt   # to a file
```

`--log-stages` on a non-profiling build says so rather than silently
doing nothing:

```
--log-stages needs a build with -DINFER_PROFILE (make linux-profile /
windows-profile / solaris-profile); this build has no stage timers
compiled in
```

Without `-DINFER_PROFILE` every timer macro expands to nothing — not a
branch, not an instruction:

```sh
nm infer-1.14.0-linux         | grep -c prof_add    # 0
nm infer-1.14.0-linux-profile | grep -c prof_add    # 1
```

See [PROFILING.md](PROFILING.md) for how to read the output.

---

## 6. Solaris / SPARC

**A separate world.** Different compiler, different option spellings,
big-endian, 64-bit, no MMX. Nothing here constrains the x86 targets.

Tested against **Sun Studio 12.3** on UltraSPARC.

```sh
make solaris              # Sun Studio, 64-bit    (recommended)
make solaris-profile      # ...with stage timers
make solaris-gcc          # GCC on Solaris, 64-bit
make solaris-gcc-profile  # ...with stage timers
make solaris-test         # the test programs
```

`make solaris` runs:

```sh
cc -Xc -xc99=none -xO5 -xunroll=16 -m64 -xtarget=native \
   -DINFER_BIG_ENDIAN=1 \
   -o infer-1.14.0-solaris \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c \
   -lm -lsocket -lnsl
```

### Three things that are easy to get wrong

**1. `-lsocket -lnsl`.** Solaris keeps the socket API outside libc.
Without these you get undefined `socket`, `bind`, `gethostbyname` at
link time. This is the usual reason a hand-rolled GCC build fails on
Solaris where Sun Studio succeeds.

**2. Sun Studio option spellings.** `-Xc -xc99=none` is strict C90
(GCC's `-std=c89 -pedantic`), `-xO5` is the optimisation level,
`-xtarget=` is `-march=`. GCC's spellings are not recognised by `cc`.

**3. Build 64-bit, and do NOT pass `-D_FILE_OFFSET_BITS=64`.**

This one broke a real build, twice, and the reason is genuinely obscure.
In strict C90 mode `-Xc` undefines `_LONGLONG_TYPE`, because C90 has no
`long long`. Solaris's `<sys/types.h>` then falls back to:

```c
typedef union { double _d; int32_t _l[2]; } longlong_t;
```

With `_FILE_OFFSET_BITS=64` on a **32-bit** build, `off_t` becomes that
union. You cannot cast to it, assign an integer to it, or do arithmetic
on it, so `mmap()`'s offset argument becomes unusable:

```
"src/sys_posix.c", line 59: argument #6 is incompatible with prototype:
    prototype: union {double *d, array[2] of int* l} : "/usr/include/sys/mman.h", line 168
    argument : long
```

In LP64 (`-m64`) `off_t` is already a plain 64-bit `long`, large-file
support is automatic, and the union never appears. Every machine these
targets are aimed at — UltraSPARC IIi and later — is 64-bit, so `-m64`
is both correct and simpler.

If you genuinely need a 32-bit Solaris build, drop `-Xc` (use
`-xc99=none` alone) so `_LONGLONG_TYPE` stays defined.

### Optimisation flags

Defaults are `-xO5 -xunroll=16`, adopted from field use on an
UltraSPARC. Override with `SUNOPT`:

```sh
make solaris SUNOPT="-xO3"
make solaris SUNOPT="-xO5 -xunroll=8"
```

Two flags you may see recommended are **not** enabled by default:

| flag | why not |
|---|---|
| `-fast` | A macro enabling `-fsimple=2` and `-fns`: non-IEEE float, reassociation, flush-to-zero. The k-quant scales are fp16 values near zero and the DeltaNet recurrence accumulates over 128 steps, so this is a genuine correctness risk. |
| `-xvis` | Enables VIS intrinsics. There is no VIS code in this project, so it does nothing. Harmless. |

To try `-fast` anyway:

```sh
make solaris-fast
./build/t_backend model.gguf     # check rel-l2 against the ref column
```

`rel-l2` should stay near `3.6e-03` for i8. If it grows, `-fast` is
changing your results.

### VIS kernels (UltraSPARC)

A fourth backend using the Visual Instruction Set. Opt-in, exactly like
MMX on x86.

```sh
gmake solaris-vis            # Sun Studio
gmake solaris-gcc-vis        # GCC
gmake solaris-vis-profile    # with stage timers
```

`solaris-vis` adds `-xarch=sparcvis -xvis -DINFER_HAVE_VIS`; the GCC
target uses `-mcpu=ultrasparc -mvis`. The file supports both compilers:
Sun Studio via `<vis_proto.h>`, GCC via `__builtin_vis_*`.

The two spellings are genuinely different languages. Sun declares the
intrinsics over plain `float` and `double` — a VIS register *is* an FP
register, and there are no vector types — while GCC uses
`__attribute__((vector_size))` with subscriptable lanes. Only the type
layer differs; the kernels are written once against four macros.

**Check the assumptions before trusting it.** The kernels rely on an
identity — `fmul8x16(w, x << 8) == w * x` exactly — that cancels the
graphics scaling built into the instruction. Prove it on your machine:

```sh
gmake solaris-vis-test       # Sun Studio
gmake solaris-gcc-vis-test   # GCC
./build/t_vis
```

```
checked 16320 (weight, activation) pairs
fmul8x16(w, x << 8) == w * x, exactly                    ok
...
all VIS assumptions hold
```

Then check the kernels themselves. This one needs no model — it builds
synthetic blocks and compares against the scalar `i8` kernel:

```sh
./build/t_viskern
```

```
Q4_K     ncols=3584 rows=4    worst rel err 0.000e+00  ok
Q5_K     ncols=3584 rows=4    worst rel err 0.000e+00  ok

kernels agree with the reference
```

Anything other than `0.000e+00` is a bug. The exactness identity above
means VIS and `i8` must agree *bit for bit*, not merely closely.

The comparison is deliberately against `qmv_i8` rather than `qmv_ref`.
Against the exact F32 reference the VIS kernels differ by 1–12%, but
that is the cost of quantising activations to int8 and the `i8` backend
shows the same spread. Comparing against `qmv_ref` will make a correct
kernel look broken.

Finally, with a model:

```sh
./build/t_backend model.gguf
```

`vis` must show the same `rel-l2` as `i8` (about 3.6e-03 for Q4_K). A
larger value means a kernel bug, not a rounding difference.

Q6_K, Q8_0 and Q4_0 have no VIS kernel and fall through to `i8`, so a
missing kernel can never cause a wrong answer.

### Cross-building on a fast machine for a slow one

```sh
make solaris SUNTARGET=ultra2      # build on a Blade 2500 for an Ultra 10
```

`SUNTARGET` defaults to `native`. Valid values include `ultra`,
`ultra2`, `ultra3`, `ultraT1`, `generic`.

### Verify

```sh
./infer-1.14.0-solaris --backend list
```
```
CPU: unknown  features:

available backends:
  i8    ok  integer kernels, portable C (recommended)   <- selected
  ref   ok  scalar float reference, maximum portability (486-safe)
```

`CPU: unknown` is correct — CPUID is x86-only. There is no `mmx` entry
because `backend_mmx.c` is x86 assembly and is not compiled here.

`i8` is portable C89 and is **6.5–8.8× faster than `ref`** on SPARC
(measured per weight format). There is no VIS backend yet.

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
   -O2 -static -o infer-1.14.0-s390x-linux \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c -lm

qemu-s390x-static ./infer-1.14.0-s390x-linux run model.gguf \
    -p "The capital of France is" -n 8 -t 0 --seed 1 -c 256
```

### SPARC64 (Linux, **not** Solaris)

```sh
sudo apt-get install -y gcc-sparc64-linux-gnu qemu-user-static

sparc64-linux-gnu-gcc -std=c89 -pedantic -Wall -Wextra -Wno-unused-parameter \
   -O2 -static -D_FILE_OFFSET_BITS=64 -o infer-1.14.0-sparc64-linux \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c src/tokenizer.c \
   src/sampler.c src/quant.c src/backend.c src/gguf.c src/json.c \
   src/util.c src/prof.c src/net_posix.c src/sys_posix.c -lm

qemu-sparc64-static ./infer-1.14.0-sparc64-linux run model.gguf \
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

## 8. Tests

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

## 9. Every target, every flag

### The four you normally want

| target | output | key flags |
|---|---|---|
| `make linux` | `infer-1.14.0-linux` | `-std=gnu89 -m32 -march=i486 -mtune=geode -mmmx -DINFER_HAVE_MMX -D_FILE_OFFSET_BITS=64` |
| `make linux-profile` | `infer-1.14.0-linux-profile` | as above `+ -DINFER_PROFILE` |
| `make windows` | `infer-1.14.0-windows.exe` | as above, `-D_WIN32_WINNT=0x0501 -lws2_32`, win32 platform files |
| `make windows-profile` | `infer-1.14.0-windows-profile.exe` | as above `+ -DINFER_PROFILE` |
| `make all` | all four | |

All four contain `mmx`, `i8` and `ref`, and all four are i486-baseline.

### Solaris / SPARC

| target | output | key flags |
|---|---|---|
| `make solaris` | `infer-1.14.0-solaris` | `-Xc -xc99=none -xO5 -xunroll=16 -m64 -xtarget=native -DINFER_BIG_ENDIAN=1 -lsocket -lnsl` |
| `make solaris-profile` | `infer-1.14.0-solaris-profile` | as above `+ -DINFER_PROFILE` |
| `make solaris-gcc` | `infer-1.14.0-solaris` | GCC equivalent, `-m64 -lsocket -lnsl` |
| `make solaris-gcc-profile` | `infer-1.14.0-solaris-profile` | as above `+ -DINFER_PROFILE` |
| `make solaris-fast` | `infer-1.14.0-solaris` | `+ -fast` — non-IEEE float, opt-in, verify with `t_backend` |
| `make solaris-test` | `build/t_*` | test programs, Sun Studio |

### Utility

| target | effect |
|---|---|
| `make test` | eight test programs into `build/` |
| `make bench` | just `build/t_backend` |
| `make help` | the target list |
| `make version` | prints `1.14.0` |
| `make checkversion` | asserts the Makefile matches `src/infer.h` |
| `make clean` | removes every build product, including other versions |

### Overrides

| variable | default | use |
|---|---|---|
| `SUFFIX` | `-1.14.0` | `make SUFFIX= linux` for an unversioned scratch build |
| `SUNOPT` | `-xO5 -xunroll=16` | Sun Studio optimisation flags |
| `SUNTARGET` | `native` | `make solaris SUNTARGET=ultra2` to cross-build |
| `SUNBITS` | `-m64` | do not change unless you know why (see section 6) |
| `CC` / `WINCC` | `cc` / `i686-w64-mingw32-gcc` | alternate compilers |

## 10. Compile-time toggles

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

## 11. Verifying a build you did not make

Everything below runs on the binary, not the source.

```sh
# 1. it is the architecture you expect
file infer-1.14.0-linux

# 2. version
./infer-1.14.0-linux --version

# 3. which kernels are compiled in, and which the CPU can use
./infer-1.14.0-linux --backend list

# 4. no post-486 instructions
objdump -d infer-1.14.0-linux --no-show-raw-insn \
  | grep -oE '^\s+[0-9a-f]+:\s+[a-z0-9.]+' | awk '{print $2}' | sort -u \
  | grep -cE '^(cmov[a-z]*|pmaddwd|paddd|punpck[a-z]+|movq|emms)$'      # want 0

# 5. no CMOV in our code
objdump -d infer-1.14.0-linux --no-show-raw-insn \
  | grep -cE '^\s+[0-9a-f]+:\s+cmov[a-z]+'                              # want 0

# 6. MMX constants 8-byte aligned (run on the target machine)
make test && ./build/t_align                    # "all MMX constants correctly aligned"
readelf -S infer-1.14.0-linux | grep -A1 '\.rodata'   # last column >= 8

# 7. Windows: three DLLs
i686-w64-mingw32-objdump -p infer-1.14.0-windows.exe | grep -c 'DLL Name'   # want 3

# 8. both backends agree bit-for-bit (greedy decoding)
./infer-1.14.0-linux run model.gguf -p "test" -n 20 -t 0 --seed 1 --backend mmx
./infer-1.14.0-linux run model.gguf -p "test" -n 20 -t 0 --seed 1 --backend i8
```

---

## 12. Troubleshooting

**`fatal error: bits/libc-header-start.h: No such file or directory`**

32-bit headers missing. `make linux` is a 32-bit build, so this is
needed even on an x86-64 host:

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
The Makefile is POSIX-clean as of 1.14.0 — if this happens, report it.
Workaround:

```sh
gmake solaris
```

---

## 13. Porting to a new platform

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
| `make linux` | ~4 s |
| `make windows` | ~5 s |
| `make all` (all four) | ~16 s |
| `make test` | ~10 s |

There are no object files and no incremental build: every target
compiles all sources in one command. At this size that is faster than
managing dependencies, and it makes each target a single copy-pasteable
line.

## See also

- [README.md](README.md) — what this is
- [USER-GUIDE.md](USER-GUIDE.md) — every command and flag
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the pieces fit
- [docs/FINDINGS.md](docs/FINDINGS.md) — the surprises, with measurements
- [PROFILING.md](PROFILING.md) — reading the performance output
