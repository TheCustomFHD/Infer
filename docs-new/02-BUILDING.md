# Building infer

- [The 60-second version](#the-60-second-version)
- [Three targets, everything else is a flag](#three-targets-everything-else-is-a-flag)
- [Toolchains](#toolchains)
- [The two-stage AVX2 build](#the-two-stage-avx2-build)
- [Verbatim recipes without make](#verbatim-recipes-without-make)
- [Compile-time defines](#compile-time-defines)
- [Solaris / SPARC](#solaris--sparc)
- [Other big-endian targets](#other-big-endian-targets)
- [Verifying a build](#verifying-a-build)
- [Running the Windows binaries](#running-the-windows-binaries)
- [Build troubleshooting](#build-troubleshooting)

---

## The 60-second version

```sh
make all          # the four shipped binaries
make test         # 9 test programs (some need a model)
make i8-split-test # 8 host-side kernel tests, no model needed
make help         # the authoritative target/flag list
```

`make all` produces exactly four artifacts:

```
infer-<version>-linux           32-bit, i486 baseline
infer-<version>-linux64         64-bit
infer-<version>-windows.exe     32-bit, XP and later
infer-<version>-windows64.exe   64-bit
```

All four are i486-baseline and carry `avx2`, `mmx`, `i8` and `ref`,
selected at run time. **There is no ISA ladder to choose from.** That
existed once and was removed in 1.23.0's ancestor 1.21.0: it made the
user diagnose their own CPU, and a wrong download died with an illegal
instruction instead of running slower.

## Three targets, everything else is a flag

```sh
make linux      # -> infer-<v>-linux        (avx2 + mmx + i8 + ref)
make windows    # -> infer-<v>-windows.exe  (same set)
make solaris    # -> infer-<v>-solaris      (vis + i8 + ref)
```

### Flags

| flag | effect |
|---|---|
| `BITS=64` | 64-bit build (default 32-bit, which runs on a 486) |
| `PROFILE=1` | per-stage timers. **Every shipped build has them**; the filename does not change because `PROFSUF` is empty |
| `NO_THREADS=1` | compile the thread pool away: no `pthread_create`, no `-lpthread` |
| `NATIVE=1` | `-march=native`. **Runs only on the build machine**; never shipped |
| `NO_MMX=1` | leave the MMX kernels out |
| `NO_AVX2=1` | leave the AVX2 kernel out (also drops the gated object) |
| `NO_VIS=1` | leave the VIS kernels out (Solaris) |
| `TOOLCHAIN=gcc` | build Solaris with GCC instead of Sun Studio |
| `FAST=1` | Solaris only: `-fast`, **non-IEEE** float. Opt-in; verify with `t_backend` |

They combine freely:

```sh
make linux PROFILE=1 BITS=64
make linux NO_THREADS=1
make solaris TOOLCHAIN=gcc NO_VIS=1
```

Each `NO_*` flag has been verified to actually change the binary. A
`NO_SSE2=1` flag existed until 1.23.3 and was **removed**: no source
file read `INFER_HAVE_SSE2`, and the binary was byte-identical with
and without it. A flag that does nothing is worse than no flag.

### Overrides

| variable | default | use |
|---|---|---|
| `CC` | `cc` | host compiler |
| `WINCC` | `i686-w64-mingw32-gcc` | 32-bit Windows |
| `WINCC64` | `x86_64-w64-mingw32-gcc` | 64-bit Windows |
| `SUFFIX` | `-<version>` | `make SUFFIX= linux` for an unversioned scratch build |
| `PROFSUF` | empty | set to `-profile` to keep timed and untimed builds side by side |
| `BUILD` | `build` | directory for test binaries and the gated AVX2 object |
| `SUNOPT` | `-xO5 -xunroll=16` | Sun Studio optimisation flags |
| `SUNTARGET` | `generic` (`native` with `NATIVE=1`) | Studio `-xtarget` |

`make printvar VAR=WINCC64` prints any of them — CI uses this to
derive compiler names rather than duplicating them.

## Toolchains

```sh
# Debian / Ubuntu, everything at once
sudo apt-get install -y \
  gcc-multilib \
  gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686 \
  gcc-mingw-w64-x86-64-win32 binutils-mingw-w64-x86-64 \
  gcc-sparc64-linux-gnu libc6-dev-sparc64-cross \
  gcc-s390x-linux-gnu qemu-user-static
```

**Use the `-win32` mingw variant, not `-posix`.** The posix-threads
build links `libwinpthread-1.dll`, which breaks the three-DLL
guarantee. Check the *binary*, not the compiler banner:

```sh
i686-w64-mingw32-gcc -o /tmp/probe.exe /tmp/hello.c
objdump -p /tmp/probe.exe | grep -ci libwinpthread     # must be 0
```

`gcc-multilib` is needed even on x86-64, because `make linux` is a
**32-bit** build. Without it you get
`fatal error: bits/libc-header-start.h: No such file or directory`.

## The two-stage AVX2 build

`backend_avx2.c` is the **only** file compiled with `-mavx2`. It
becomes its own object and is linked against everything else, which is
built at the i486 baseline. That is what lets one binary boot on a 486
and use `VPMADDUBSW` on a Haswell.

Compiling everything in one command with `-mavx2` would apply it
project-wide and the binary would fault on a 486 before reaching
`main()`.

Three things are easy to get wrong, each failing differently:

| omission | symptom |
|---|---|
| `-DINFER_A2_LINKED` in stage 2 | `multiple definition of 'bk_a2_amax'` — the stub and the real object both provide it |
| the object, but keeping the macro | `undefined reference to 'bk_a2_amax'` |
| `-DINFER_HAVE_THREADS` (and `-lpthread`) | links and runs, but silently single-threaded: `-T 4` reports 1 |

Registration keys on **`INFER_HAVE_AVX2`, never `__AVX2__`**.
`backend.c` is compiled at the i486 baseline, so `__AVX2__` is false
there even in a build that links the AVX2 object — testing it silently
drops the kernel from every artifact. This happened once and is now
asserted in CI.

## Verbatim recipes without make

Exactly what `make -n linux` prints, so it can be pasted and run.
Verified by executing it in a clean directory.

### Linux 32-bit

```sh
mkdir -p build
cc -std=gnu89 -Wall -Wextra -Wno-unused-parameter \
   -O3 -march=haswell -mavx2 -ffp-contract=off -mmmx \
   -DINFER_HAVE_MMX -DINFER_HAVE_AVX2 -DINFER_HAVE_THREADS \
   -m32 -D_FILE_OFFSET_BITS=64 -Isrc \
   -c -o build/backend_avx2.o src/backend_avx2.c

cc -std=gnu89 -Wall -Wextra -Wno-unused-parameter \
   -O3 -march=i486 -mtune=generic -mno-sse -mno-sse2 -mfpmath=387 -mmmx \
   -DINFER_HAVE_MMX -DINFER_HAVE_AVX2 -DINFER_HAVE_THREADS \
   -DINFER_A2_LINKED -m32 -D_FILE_OFFSET_BITS=64 \
   -o infer-<version>-linux \
   src/main.c src/opts.c src/server.c src/chat.c src/agent.c \
   src/jinja.c src/jinja_eval.c src/mcp.c src/qwen35.c \
   src/tokenizer.c src/sampler.c src/quant.c src/backend.c \
   src/gguf.c src/json.c src/util.c src/prof.c src/tpool.c \
   src/backend_a2stub.c src/net_posix.c src/sys_posix.c \
   src/backend_mmx.c build/backend_avx2.o \
   -lm -lpthread
```

### Linux 64-bit

Same, but `-m64` in both stages, drop `-D_FILE_OFFSET_BITS=64` (only
needed on 32-bit hosts), and the baseline arch flags collapse to
`-mtune=generic`.

### Windows 32-bit

Identical to Linux 32-bit except: `i686-w64-mingw32-gcc`,
`-D_WIN32_WINNT=0x0501` in both stages, `net_win32.c`/`sys_win32.c`
instead of the POSIX pair, and `-lws2_32` instead of `-lm -lpthread`.

**No `-lpthread`**: the pool uses `CreateThread` and Win32 events
directly, so threading costs no extra library on Windows.

### Why each flag is there

| flag | reason |
|---|---|
| `-std=gnu89` | `backend_mmx.c` uses GNU inline asm — the only file that is not strict C89 |
| `-march=i486` | baseline. No CMOV (a Pentium Pro instruction that faults on a 486) |
| `-mtune=generic` | **scheduling only**, never changes the instruction set. Was `-mtune=geode` until the 64-bit targets needed one shared value |
| `-mno-sse -mno-sse2 -mfpmath=387` | keeps the 32-bit build off SSE, which a 486 does not have |
| `-mmmx` | lets the compiler use MMX *inside `backend_mmx.c`*, behind a runtime CPUID check |
| `-ffp-contract=off` | **required.** `-march=haswell` otherwise fuses `a*b+c` into `vfmadd`, diverging from the other backends at around token 8. Costs 2.8% |
| `-D_FILE_OFFSET_BITS=64` | 64-bit `off_t` on 32-bit hosts; needed for models >2 GB |
| `-D_WIN32_WINNT=0x0501` | pins the Windows API to XP so the compiler refuses anything newer |

## Compile-time defines

| define | effect |
|---|---|
| `-DINFER_HAVE_MMX` | compiles `backend_mmx.c` in and registers `mmx`. Needs `-mmmx` and `-std=gnu89` |
| `-DINFER_HAVE_AVX2` | registers `avx2`. **Must be this macro, never `__AVX2__`** |
| `-DINFER_A2_LINKED` | tells `backend_a2stub.c` to stand down; set by the Makefile, not derivable from `__AVX2__` |
| `-DINFER_HAVE_THREADS` | compiles the thread pool in. Needs `-lpthread` on POSIX |
| `-DINFER_HAVE_VIS` | compiles `backend_vis.c` in and registers `vis` (SPARC) |
| `-DINFER_VIS_Q6K` | SPARC: the VIS Q6_K kernel. **On by default** since 1.25.0 (`make solaris` adds it) — worth −29% on `matvec Q6_K` and 13.87 → 9.48 s/tok on an UltraSPARC IIi. Findings 30 and 59, which kept it off, are retracted; see finding 61 |
| `-DINFER_PROFILE` | per-stage timers. Without it every timer macro expands to nothing |
| `-DINFER_BIG_ENDIAN=1`/`=0` | forces byte order. Autodetected; the build **fails** rather than guessing |
| `-DINFER_BACKEND="i8"` | changes the default backend when none is given |
| `-DINFER_VERSION="..."` | overrides the version string |

## Solaris / SPARC

A separate world: different compiler, different flags, big-endian,
64-bit.

```sh
gmake solaris                       # Sun Studio, 64-bit (recommended)
gmake solaris PROFILE=1             # with timers
gmake solaris TOOLCHAIN=gcc         # GCC instead
gmake solaris FAST=1                # non-IEEE float, opt-in
gmake solaris-test                  # VIS assumption tests, Studio
```

### Optimisation flags on SPARC

`make solaris TOOLCHAIN=gcc` defaults to **`-O2 -funroll-loops`**, not
`-O3`. That is deliberate and measured.

VIS operands live in the floating-point register file, and UltraSPARC
has **no direct move between the integer and FP files** — every
integer-side value fed to a VIS instruction costs a store plus a load.
`-O3` unrolls the inner loop 4x, multiplies the number of such values
live at once, and the allocator spills. Per `fmul8x16` (so the unroll
factor cannot flatter the count), on `vis_dot_q4_K`:

| flags | insns/mul | stack-spill ops/mul |
|---|---:|---:|
| `-O3` | 19.5 | 4.2 |
| `-O2` | 46.0 | 10.5 |
| `-Os` | 38.8 | 9.2 |
| **`-O2 -funroll-loops`** | **16.0** | **3.7** |

Override to compare on your own silicon:

```sh
gmake solaris TOOLCHAIN=gcc SOLGCCOPT=-O3
```

Sun Studio is unaffected; `SUNOPT` still controls it.

Things that are easy to get wrong:

- **Build 64-bit.** In LP64 `off_t` is a plain `long`. In 32-bit with
  `-Xc`, Studio undefines `_LONGLONG_TYPE` and `off_t` becomes a union
  you cannot assign to.
- **The Makefile has no conditionals.** Solaris `/usr/ccs/bin/make`
  has no `ifeq`, so every switch uses indirect variable expansion
  (`$(VAR_$(FLAG))`). Do not "tidy" that into `ifeq`.
- **`CC ?= cc` cannot override make's built-in default.** Make assigns
  `CC = cc` before reading the Makefile, so `?=` never fires. On
  Solaris `cc` is Studio, so GCC targets silently called the wrong
  compiler. The fix is a separate `GNUCC` variable.
- **`-fast` is non-IEEE.** Verify with `t_backend` before trusting
  output.

## Other big-endian targets

```sh
# s390x under qemu
s390x-linux-gnu-gcc -std=c89 -O2 -static -Isrc -o t_endian_be \
  tests/t_endian.c src/gguf.c src/quant.c src/backend.c \
  src/util.c src/prof.c src/tpool.c src/backend_a2stub.c \
  src/sys_posix.c -lm
qemu-s390x-static ./t_endian_be

# sparc64 (Linux, not Solaris)
sparc64-linux-gnu-gcc -std=gnu89 -O2 -Isrc -DINFER_HAVE_THREADS \
  -DINFER_BIG_ENDIAN=1 -o t_thread_sparc tests/t_thread.c \
  src/backend.c src/quant.c src/util.c src/prof.c src/gguf.c \
  src/tpool.c src/sys_posix.c src/backend_a2stub.c -lm -lpthread
qemu-sparc64-static -L /usr/sparc64-linux-gnu ./t_thread_sparc 4
```

## Verifying a build

Everything here runs on the binary, not the source.

```sh
V=1.23.2

# 1. architecture and subsystem
file infer-$V-linux
objdump -p infer-$V-windows.exe | grep -E 'MajorSubsystem|MinorSubsystem'   # 4 / 0

# 2. version
./infer-$V-linux --version

# 3. which kernels are in, and which this CPU can use
./infer-$V-linux --backend list

# 4. Windows: exactly three XP-safe DLLs
objdump -p infer-$V-windows.exe | grep 'DLL Name'
#   KERNEL32.dll / msvcrt.dll / WS2_32.dll

# 5. the i486 baseline holds: no CMOV or SSE/AVX outside the gated object
#    (compile each baseline source alone and check)
objdump -d build/backend_avx2.o --no-show-raw-insn | grep -c '%ymm'   # >0
# every other object must give 0 for: cmov, %xmm, %ymm

# 6. NO_THREADS really removes the pool
make clean && make linux NO_THREADS=1
nm infer-$V-linux | grep -c pthread_create     # 0

# 7. backends agree bit-for-bit under greedy decoding
./infer-$V-linux run model.gguf -n 8 -t 0 --seed 1 --backend mmx -c 256
./infer-$V-linux run model.gguf -n 8 -t 0 --seed 1 --backend i8  -c 256
```

## Running the Windows binaries

**Wine works.** This was written off for four releases on the strength
of one bootstrap failure, and that call was wrong — the failure was a
prefix in a directory the user did not own. A Windows-only crash
shipped twice behind that assumption (finding 55).

```sh
sudo apt-get install -y wine
export WINEPREFIX="$HOME/.cache/wineprefix"   # NOT /tmp; must be yours
export WINEDEBUG=-all
wine ./infer-<version>-windows64.exe --backend list
wine ./infer-<version>-windows64.exe run 'Z:\path\to\model.gguf' \
     -n 12 -t 0 -p "whats the capital of germany" -T 2
```

Four rules, each learned the hard way:

1. **`WINEPREFIX` must be a directory you own.** `/tmp` fails with
   `wine: '/tmp' is not owned by you`.
2. **Put the prefix somewhere excluded from workspace snapshots**
   (`~/.cache/…`). It grows to **1.4 GB** and will otherwise eat the
   workspace budget and get your model evicted mid-benchmark.
3. **Model paths must be Windows paths.** `Z:\` maps to `/`.
4. **Kill stragglers between runs.** An aborted Wine run leaves a
   process at ~97% CPU that silently halves every subsequent
   measurement:
   ```sh
   pkill -9 wineserver; pkill -9 -f '\.exe'
   ```

**Test every thread count**, not just the default. The 1.23.0 Windows
crash was invisible at `-T 1` because that path never enters the pool:

```sh
for T in 1 2 4 8; do wine ./infer-<v>-windows64.exe run "$M" -n 12 -t 0 -p hi -T $T; done
```

## Build troubleshooting

| symptom | cause / fix |
|---|---|
| `bits/libc-header-start.h: No such file` | install `gcc-multilib`; `make linux` is a 32-bit build |
| four DLLs, one is `libwinpthread-1.dll` | posix-threads mingw; install the `-win32` variant |
| `multiple definition of 'bk_a2_amax'` | missing `-DINFER_A2_LINKED` in stage 2 |
| `undefined reference to 'bk_a2_amax'` | the macro is set but the AVX2 object is not linked |
| `-T 4` reports 1 thread | built without `-DINFER_HAVE_THREADS` |
| illegal instruction on an old CPU | a non-baseline flag leaked outside the gated object; check step 5 above |
| Studio: `-W option with unknown program all` | Studio is being handed GCC flags; see the `CC ?= cc` trap |
| output differs between backends | expected for `avx2` on k-quants; not expected for `i8`/`mmx`/`vis` |
