# Compilation guide

Atomic, step-by-step, for every target. Follow a section top to bottom
and it will work; every command here was executed on a clean Debian 13
machine before being written down.

There are no submodules, no CMake, no `pip install`, no vendored tree.
The whole project is 20 `.c`/`.h` files and one Makefile.

---

## Contents

- [0. The 30-second version](#0-the-30-second-version)
- [1. Prerequisites](#1-prerequisites)
- [2. Native build](#2-native-build-linux--macos)
- [3. 32-bit i486](#3-32-bit-i486)
- [4. Geode LX / Pentium MMX](#4-geode-lx--pentium-mmx)
- [5. Windows XP](#5-windows-xp)
- [6. Profiling builds](#6-profiling-builds)
- [7. Tests](#7-tests)
- [8. Every target, every flag](#8-every-target-every-flag)
- [9. Compile-time toggles](#9-compile-time-toggles)
- [10. Verifying a build](#10-verifying-a-build)
- [11. Troubleshooting](#11-troubleshooting)
- [12. Porting to a new platform](#12-porting-to-a-new-platform)

---

## 0. The 30-second version

```sh
sudo apt update
sudo apt install -y build-essential
make
./infer --version
```

That is the whole native build. Everything below is for cross-targets
and options.

---

## 1. Prerequisites

| package | needed for | approx size |
|---|---|---|
| `build-essential` | any build | 250 MB |
| `gcc-multilib` | `i486`, `geode` (32-bit on x86-64) | 30 MB |
| `gcc-mingw-w64-i686-win32` | `windows`, `windows-mmx` | 120 MB |

```sh
sudo apt update
sudo apt install -y build-essential gcc-multilib gcc-mingw-w64-i686-win32
```

**Notes that matter:**

* `gcc-multilib` pulls in `libc6-dev-i386` and `lib32gcc-14-dev`
  automatically. Do **not** install those by hand.
* Use the **`-win32`** MinGW variant, not `-posix`. The win32-threads
  build links against nothing but the three system DLLs — verified with
  `objdump -p`. The posix variant may add a `libwinpthread-1.dll`
  dependency.
* The `mingw-w64` metapackage also works but drags in the x86-64 Windows
  toolchain — ~250 MB you will not use.
* **Python is not needed to build.** `tools/gen_unicode.py` regenerates a
  header that is already checked in; `tools/ref_numpy.py` and
  `tools/mcp_test_server.py` are development-only.

### Other distributions

| distro | command |
|---|---|
| Fedora / RHEL | `sudo dnf install gcc make glibc-devel.i686 mingw32-gcc` |
| Arch | `sudo pacman -S base-devel lib32-glibc mingw-w64-gcc` |
| Alpine | `apk add build-base` (32-bit/Windows need extra work) |
| macOS | `xcode-select --install`, then `make` (native only) |

---

## 2. Native build (Linux / macOS)

```sh
# 1. dependencies
sudo apt install -y build-essential

# 2. build
make

# 3. confirm
./infer --version          # -> infer 1.8.0
./infer --backend list     # -> your CPU's features and chosen backend
```

**Expected:** one `cc` line, no warnings, an `infer` binary of ~160 KB.

Compiles clean under `-std=c89 -pedantic -Wall -Wextra`.

### Get a model and run it

```sh
curl -L -o Qwen3.5-0.8B-Q4_K_M.gguf \
  https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf

./infer run Qwen3.5-0.8B-Q4_K_M.gguf -p "What is the capital of France?" -n 10 -t 0
```

Expected output: `The capital of France is **Paris**.`

`-t 0` is greedy, so that string is deterministic — it is the fastest
way to confirm a new build is sane. **All five binaries produce it.**

---

## 3. 32-bit i486

For a 486, or any 586 without MMX.

```sh
# 1. 32-bit toolchain
sudo apt install -y gcc-multilib

# 2. build
make i486

# 3. confirm
file infer-i486
# -> ELF 32-bit LSB executable, Intel i386
```

Flags used:

```
-m32 -march=i486 -mtune=i486
-mno-sse -mno-sse2 -mno-mmx -mfpmath=387
-D_FILE_OFFSET_BITS=64
```

`-D_FILE_OFFSET_BITS=64` lets `mmap()` handle offsets past 2 GB. Not
strictly needed for a 0.5 GB model; costs nothing and avoids grief with
larger ones.

**Verify no post-486 instruction leaked in:**

```sh
objdump -d infer-i486 --no-show-raw-insn \
  | grep -oE '^\s+[0-9a-f]+:\s+[a-z0-9.]+' | awk '{print $2}' | sort -u \
  | grep -E '^(pmaddwd|paddd|punpck|pand|psrlw|emms|movq|movd|movap|cvtt?s|cmov|prefetch|pf[a-z]+)$'
```

Prints **nothing** on a correct build.

---

## 4. Geode LX / Pentium MMX

Adds the MMX backend. ~2× faster than `i8` on real Geode hardware.

```sh
sudo apt install -y gcc-multilib
make geode

file infer-geode          # -> ELF 32-bit LSB executable, Intel i386
```

Flags:

```
-std=gnu89 -m32 -march=geode -mtune=geode
-mmmx -mno-sse -mno-sse2 -mfpmath=387
-DINFER_HAVE_MMX -D_FILE_OFFSET_BITS=64
```

**Why `gnu89` here:** `src/backend_mmx.c` uses GNU inline assembly, which
is not ANSI C. It is the only non-conforming file in the project and is
compiled only for this target and `windows-mmx`. Everything else builds
under strict `-std=c89 -pedantic`.

**This binary still runs on a 486** — the MMX backend is gated behind a
CPUID check and `auto` falls back to `i8`.

**Verify MMX is present and nothing newer:**

```sh
objdump -d infer-geode --no-show-raw-insn \
  | grep -oE '^\s+[0-9a-f]+:\s+[a-z0-9.]+' | awk '{print $2}' | sort -u \
  | grep -E '^(pmaddwd|paddd|emms|movap|cvtt?s|pf[a-z]+)$'
```

Should print `emms paddd pmaddwd` and nothing else. Any `pf*` would mean
3DNow! crept in; any `movap`/`cvt*` would mean SSE.

---

## 5. Windows XP

```sh
# 1. cross-compiler (use the -win32 variant)
sudo apt install -y gcc-mingw-w64-i686-win32

# 2. build both
make windows        # -> infer.exe        486-safe, no MMX
make windows-mmx    # -> infer-mmx.exe    with MMX

# 3. confirm
file infer.exe
# -> PE32 executable for MS Windows 4.00 (console), Intel i386
```

**Verify it is XP-safe — three DLLs, all on a stock install:**

```sh
i686-w64-mingw32-objdump -p infer.exe | grep 'DLL Name'
```

```
        DLL Name: KERNEL32.dll
        DLL Name: msvcrt.dll
        DLL Name: WS2_32.dll
```

**Verify no post-XP API:**

```sh
i686-w64-mingw32-objdump -p infer.exe \
  | grep -Ei 'WSAPoll|getaddrinfo|inet_pton|GetTickCount64|SRWLock|CreateFile2|InitOnce'
```

Prints nothing on a correct build.

**Verify the MMX constants are aligned** (this one matters — see
[docs/FINDINGS.md](docs/FINDINGS.md#4-the-mmx-alignment-bug--invisible-on-a-dev-machine)):

```sh
i686-w64-mingw32-gcc -std=gnu89 -O2 -march=geode -mmmx -DINFER_HAVE_MMX \
    -c src/backend_mmx.c -o /tmp/w.o
i686-w64-mingw32-objdump -h /tmp/w.o | grep -E '^\s+[0-9]+ \.rdata '
```

The last column must be **`2**3`** (8-byte). `2**2` means the alignment
attribute was ignored and the binary will misbehave on real hardware.

If your MinGW is named differently:

```sh
make windows WINCC=i686-w64-mingw32-gcc-win32
```

---

## 6. Profiling builds

Separate binaries; the normal ones stay uninstrumented.

```sh
make profile           # native            -> infer-profile
make profile-i486      # 32-bit 486-safe   -> infer-i486-profile
make profile-geode     # 32-bit + MMX      -> infer-geode-profile
make profile-windows   # Win32 + MMX       -> infer-profile.exe
```

```sh
./infer-profile run model.gguf -p "Hello" -n 10 -t 0 \
    --log-perf --log-file perf.txt
```

Without `-DINFER_PROFILE` every timer macro expands to nothing —
verified with `nm`. See [PROFILING.md](PROFILING.md).

---

## 7. Tests

```sh
make test
```

Builds eight programs into `build/`:

```sh
./build/t_align                      # no model needed
./build/t_agent                      # no model needed
./build/t_endian     [model.gguf]    # model optional
./build/t_jinja      model.gguf
./build/t_quant      model.gguf
./build/t_tokenizer  model.gguf
./build/t_engine     model.gguf
./build/t_backend    model.gguf      # also a benchmark
```

Expected tails: `all MMX constants correctly aligned`,
`all passed` (t_agent), `all endian tests passed`,
`all jinja tests passed`, `all tokenizer tests passed`.

### Big-endian hosts

Nothing special is needed — the normal targets are byte-order neutral.
To check on a machine you do not have:

```sh
sudo apt install gcc-s390x-linux-gnu qemu-user-static

s390x-linux-gnu-gcc -std=c89 -O2 -static -o t_endian_be \
  tests/t_endian.c src/gguf.c src/quant.c src/backend.c \
  src/util.c src/prof.c src/sys_posix.c -lm
qemu-s390x-static ./t_endian_be model.gguf
```

The tensor lines it prints must match a little-endian run exactly.
`INFER_BIG_ENDIAN` is detected automatically; override with
`-DINFER_BIG_ENDIAN=1` or `=0` if your compiler is not recognised.

`make bench` builds just `t_backend`.

---

## 8. Every target, every flag

| target | output | flags |
|---|---|---|
| `make` | `infer` | `-std=c89 -pedantic -Wall -Wextra -O2` |
| `make i486` | `infer-i486` | `-m32 -march=i486 -mno-sse -mno-mmx -mfpmath=387 -D_FILE_OFFSET_BITS=64` |
| `make geode` | `infer-geode` | `-std=gnu89 -m32 -march=geode -mmmx -DINFER_HAVE_MMX` |
| `make windows` | `infer.exe` | `-march=i486 -D_WIN32_WINNT=0x0501 -lws2_32` |
| `make windows-mmx` | `infer-mmx.exe` | `+ -mmmx -DINFER_HAVE_MMX` |
| `make profile` | `infer-profile` | `+ -DINFER_PROFILE` |
| `make profile-i486` | `infer-i486-profile` | i486 `+ -DINFER_PROFILE` |
| `make profile-geode` | `infer-geode-profile` | geode `+ -DINFER_PROFILE` |
| `make profile-windows` | `infer-profile.exe` | windows-mmx `+ -DINFER_PROFILE` |
| `make test` | `build/t_*` | eight test programs |
| `make bench` | `build/t_backend` | benchmark only |
| `make clean` | — | removes all of the above |

`-D_WIN32_WINNT=0x0501` pins the Windows API surface to XP, so the
compiler refuses anything newer.

---

## 9. Compile-time toggles

| define | effect |
|---|---|
| `INFER_HAVE_MMX` | compile the MMX backend (needs `backend_mmx.c` in the link) |
| `INFER_PROFILE` | compile per-stage timers |
| `INFER_BACKEND="i8"` | set the default backend instead of `auto` |
| `INFER_VERSION="x.y"` | override the reported version |
| `_FILE_OFFSET_BITS=64` | 64-bit file offsets on 32-bit hosts |
| `_WIN32_WINNT=0x0501` | pin to the XP API |

Examples:

```sh
make CFLAGS='-std=c89 -O2 -DINFER_BACKEND=\"i8\"'
make geode CC='gcc -Os'      # smaller code; sometimes faster on Geode
```

`-O3` was not faster in testing and inflates the binary.

---

## 10. Verifying a build

Run these after any change to the kernels.

```sh
# 1. everything builds clean
make clean && make && make test

# 2. deterministic output — all binaries agree
./infer run model.gguf -p "What is the capital of France?" -n 10 -t 0
# -> The capital of France is **Paris**.

# 3. every backend agrees
for b in ref i8 mmx; do ./infer-geode run model.gguf -p "..." -n 8 -t 0 --backend $b; done

# 4. alignment guard
./build/t_align

# 5. kernel accuracy vs the reference
./build/t_backend model.gguf     # rel-l2 ~3.6e-3 for i8/mmx is correct
```

`rel-l2 ≈ 3.6e-3` is expected — the cost of int8 activation
quantisation, far below the error 4-bit weights already introduce. `ref`
is 0 by definition.

---

## 11. Troubleshooting

**`fatal error: bits/libc-header-start.h: No such file or directory`**
32-bit headers missing → `sudo apt install gcc-multilib`

**`cannot find -lgcc`** when building 32-bit
Same cause, same fix.

**`i686-w64-mingw32-gcc: command not found`**
```sh
sudo apt install gcc-mingw-w64-i686-win32
# or point at what you have:
make windows WINCC=i686-w64-mingw32-gcc-win32
```

**The `.exe` wants `libwinpthread-1.dll`**
You used the posix-threads MinGW. Install the `-win32` variant.

**`architecture 'llama' is not supported`**
This engine implements `qwen35` only. Check with `infer info`.

**Output is garbage / NaN on a new platform**
Try `--backend ref`. If `ref` is right and a fast backend is not, that
is a kernel bug. If `ref` is also wrong on a big-endian host, run
`./build/t_endian` — it decodes known bit patterns and, given a model,
prints tensor values that must match a little-endian run byte for byte.

**Silent NaNs at `-O2` but not `-O0` after writing MMX code**
MMX registers alias the x87 stack. You need `emms` **and** the
`"st"`–`"st(7)"` clobber list, or GCC schedules float work into the
middle of your asm block. See
[docs/files/backend_mmx-c.md](docs/files/backend_mmx-c.md).

**MMX works on a modern desktop, crashes on real old hardware**
Almost certainly constant alignment. Run `./build/t_align` and check
`.rdata` is `2**3`.

---

## 12. Porting to a new platform

Only **four files** know an operating system exists:

```
src/net.h   ->  src/net_posix.c   or  src/net_win32.c
src/sys.h   ->  src/sys_posix.c   or  src/sys_win32.c
```

`net.h` is twelve functions (listen / accept / connect / read / write /
close / …). `sys.h` is four (read-only file mapping plus a monotonic
clock).

Implement those two files, add a Makefile target, change nothing else —
`server.c`, `chat.c`, `mcp.c` and the engine are byte-for-byte identical
across platforms.

If your platform has no `mmap` equivalent, return `NULL` from
`sys_map_file()`; `gguf.c` falls back to reading the model into RAM.

---

## Measured build times

Six-core x86-64, from `make clean`:

| target | time |
|---|---|
| `make` | ~3 s |
| `make i486` | ~3 s |
| `make geode` | ~3 s |
| `make windows` | ~3 s |
| `make test` | ~5 s |

~11,000 lines. It compiles quickly because there is nothing to compile
but itself.

---

## See also

- [USER-GUIDE.md](USER-GUIDE.md) · [PROFILING.md](PROFILING.md)
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) ·
  [docs/FINDINGS.md](docs/FINDINGS.md)
