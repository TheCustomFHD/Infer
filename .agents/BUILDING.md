# Building — the exact recipe

Copy-pasteable, in order, with the output you should see. Verified in a
clean Debian sandbox.

If you are an AI agent and a build has failed, start at step 1 rather
than guessing — most failures are a missing package, not a code problem.

---

## 1. Toolchain

```sh
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    gcc-multilib \
    gcc-mingw-w64-i686-win32 \
    binutils-mingw-w64-i686
```

| package | needed for |
|---|---|
| `build-essential` | anything |
| `gcc-multilib` | `make linux` — it is a **32-bit** build even on x86-64 |
| `gcc-mingw-w64-i686-win32` | `make windows` |
| `binutils-mingw-w64-i686` | `i686-w64-mingw32-objdump`, used by every Windows check |

**Re-run this in every session.** Sandboxes discard installed packages
between turns. `command not found` halfway through a session means the
packages went away, not that something broke.

### Confirm the right mingw variant

There are two mingw builds. The **posix** one links
`libwinpthread-1.dll`, which breaks the three-DLL guarantee. The
**win32** one does not.

Do not test this by grepping for `thread-model=` — some Debian builds do
not print it, and the check then fails for the wrong reason. Test the
property directly:

```sh
echo 'int main(void){return 0;}' > /tmp/probe.c
i686-w64-mingw32-gcc -o /tmp/probe.exe /tmp/probe.c
i686-w64-mingw32-objdump -p /tmp/probe.exe | grep -ci libwinpthread
```

```
0
```

`0` is correct. If you get `1`:

```sh
sudo apt-get install -y gcc-mingw-w64-i686-win32
sudo update-alternatives --config i686-w64-mingw32-gcc   # choose win32
```

---

## 2. Build

```sh
make all
```

```sh
ls -1 infer-*
```
```
infer-<version>-linux
infer-<version>-linux
infer-<version>-windows.exe
infer-<version>-windows.exe
```

Four binaries from two targets. There is no separate MMX or i486 build:
every x86 binary contains all the x86 kernels (`mmx`, `i8`, `ref`) and uses an i486 baseline, selecting one at run time.

Individually:

```sh
make linux
make linux PROFILE=1
make windows
make windows PROFILE=1
```

Flags, all combinable: `PROFILE=1`, `NATIVE=1`, `BITS=64`,
`TOOLCHAIN=gcc` (Solaris), and `NO_MMX=1` / `NO_SSE2=1` / `NO_AVX2=1` /
`NO_VIS=1` to leave a kernel out. **Do not add build targets** — there
are three (`linux`, `windows`, `solaris`) and anything else is a flag.

---

## 2b. Which build for which machine

Measured on a Xeon @ 2.60 GHz with the 0.8B Q4_K_M model, `-n 10 -t 0
--seed 1 -c 512`, generation tok/s, best of 3. Ordering transfers to
other hosts; magnitudes do not.

| target CPU | build | picks | tok/s here | notes |
|---|---|---|---|---|
| **pure 486** (no MMX) | `make linux` + musl/uClibc | `ref` | — | see the libc trap below |
| **Geode LX** (486+MMX) | `make linux` | `mmx` | 2.45 | the shipped default |
| **Pentium M / Dothan** | `make linux` | `mmx` | 2.45 | SSE2 exists but no kernel uses it yet |
| **i7 9th gen (AVX2)** | `make linux BITS=64 NATIVE=1` | `i8` | **4.23** | 1.73x; runs only on that CPU class |
| i7, portable binary | `make linux` | `mmx` | 2.45 | same binary as the Geode |

`NATIVE=1` and `BITS=64` both emit CMOV and SSE/AVX (`NATIVE=1`: 114
CMOV + 716 SSE/AVX; `BITS=64`: 126 + 2455). **Neither is 486- or
Geode-safe.** Only the default and `PROFILE=1` are.

### The libc trap on a real 486

Our object files are clean — 0 CMOV, 0 SSE/AVX across every
translation unit. **glibc is not.** A `make linux` binary, and even a
static `printf("hi")`, dies before `main()`:

```
$ qemu-i386-static -cpu 486 ./infer-<version>-linux
qemu: uncaught target signal 4 (Illegal instruction)

$ qemu-i386-static -cpu 486 -d in_asm /tmp/hello_static
IN: _dl_aux_init
0x0805eb79:  0f 42 c2   cmovbl %edx, %eax      <-- inside glibc
```

Emulated floor, default build:

```
486        Illegal instruction   (glibc _dl_aux_init)
pentium    Illegal instruction   (same)
pentium2   mmx  <- selected
pentium3   mmx  <- selected
n270       mmx  <- selected
core2duo   mmx  <- selected
Haswell    mmx  <- selected
```

So the shipped Linux binary is **Pentium II and later** in practice,
for the same reason the Windows builds are Pentium-Pro-and-later: the
C runtime, not our code. A genuine 486 needs a libc built for one —
musl or uClibc-ng, statically linked. Debian's `musl-gcc` is x86-64
only, so this has **not been verified here**; it is the known route,
not a tested one.

This mirrors the mingw CRT finding: `strtod`/`pow`/`ldexp` pull in
CMOV on Windows too. Keep asserting our own objects are clean; do not
claim the linked binary boots a 486 without testing it on one.

---

## 3. Verify the Windows build will actually load on XP

This is where agents usually stop too early. Compiling is not enough.

```sh
V=$(make version)
```

**Three DLLs, no more:**

```sh
i686-w64-mingw32-objdump -p infer-$V-windows.exe | grep 'DLL Name'
```
```
	DLL Name: KERNEL32.dll
	DLL Name: msvcrt.dll
	DLL Name: WS2_32.dll
```

All three ship with Windows XP. A fourth means something went wrong —
usually the posix mingw.

**32-bit PE:**

```sh
file infer-$V-windows.exe
```
```
PE32 executable (console) Intel 80386, for MS Windows
```

`PE32+` means you used `x86_64-w64-mingw32-gcc`. Use `i686-`.

**No post-XP API:**

```sh
i686-w64-mingw32-objdump -p infer-$V-windows.exe \
  | grep -oE '\b(GetTickCount64|InitializeCriticalSectionEx|CreateFile2|InetPton|GetAddrInfoW|SetThreadStackGuarantee)\b' \
  | sort -u
```

Must print nothing.

**MMX kernels present:**

```sh
i686-w64-mingw32-objdump -d infer-$V-windows.exe | grep -cE 'pmaddwd|punpcklbw'
```
```
206
```

---

## 4. Verify the Linux build

```sh
V=$(make version)

# no CMOV: it is a Pentium Pro instruction and faults on a 486
objdump -d infer-$V-linux --no-show-raw-insn \
  | grep -cE '^[[:space:]]+[0-9a-f]+:[[:space:]]+cmov[a-z]+'
```
```
0
```

```sh
objdump -d infer-$V-linux | grep -cE 'pmaddwd|punpcklbw|paddd'
```
```
354
```

```sh
./infer-$V-linux --backend list
```
```
CPU: GenuineIntel  features: mmx cmov

available backends:
  avx2  ok  integer kernels with AVX2 VPMADDUBSW inner loop (Haswell+)   <- selected
  mmx   ok  integer kernels with MMX inner loop (Pentium MMX / K6 / Geode+)
  i8    ok  integer kernels, portable C (recommended)
  ref   ok  scalar float reference, maximum portability (486-safe)
```

All four must be listed even in the 32-bit build. If `avx2` is missing
the two-stage compile broke, or the backend table was gated on
`__AVX2__` instead of `INFER_HAVE_AVX2` (finding 50).

---

## 5. Test

```sh
make test
./build/t_align      # MMX constant alignment -- guards a real fault
./build/t_agent      # cross-mode prompts, tool-call shapes
./build/t_endian     # byte-order neutrality
./build/t_jinja      # template engine

make i8-split-test
./build/t_ident      # every backend bit-identical (memcmp, not tolerance)
./build/t_avx2sat    # VPMADDUBSW cannot saturate on any format
./build/t_avx2acc    # AVX2 k-quant error bounded against the float ref
./build/t_thread     # every backend deterministic under threading
./build/t_thread 4   # ...and at several thread counts
```

Each ends in a line saying it passed.

`t_thread` loops over every backend **by name**. Testing only whatever
`auto` picks is what let a threading race in `mmx` reach a release
(finding 52).

With a model, the strongest test is making the backends disagree:

```sh
curl -L -o model.gguf \
  "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf"

V=$(make version)
for b in mmx i8 ref; do
  ./infer-$V-linux run model.gguf -p "The capital of France is" \
      -n 8 -t 0 --seed 1 --backend $b -c 256
done
```

`-t 0` is greedy: all three must print **identical** text. Expected:

```
The capital of France is **Paris**.
```

---

## 6. Do not try to run the .exe

Wine is unreliable in sandboxes — Debian's Wine 10 fails its own prefix
bootstrap with `user32.dll.CreateDialogParamW` unimplemented. Do not
spend time on it.

The Linux build is the *same sources with the same flags*, swapping only
`net_win32.c`/`sys_win32.c` for their POSIX equivalents. If the Linux
build is correct and the static checks above pass, the Windows build is
almost certainly fine.

---

## 7. Clean up

```sh
rm -f model.gguf     # 508 MB; sandboxes prune large workspaces
make clean
```

---

## Failure table

| symptom | cause | fix |
|---|---|---|
| `i686-w64-mingw32-gcc: command not found` | packages discarded between turns | re-run step 1 |
| `i686-w64-mingw32-objdump: command not found` | `binutils-mingw-w64-i686` missing | re-run step 1 |
| `bits/libc-header-start.h: No such file` | `gcc-multilib` missing; `make linux` is 32-bit | `sudo apt-get install -y gcc-multilib` |
| four DLLs, one is `libwinpthread-1.dll` | posix mingw | install the `-win32` variant |
| `PE32+` instead of `PE32` | used `x86_64-w64-mingw32-gcc` | use `i686-` |
| `cannot open 'model.gguf'` mid-session | sandbox pruned it | re-download |
| `--log-stages` prints nothing | non-profile build | use `make linux PROFILE=1` |
| `make: *** No rule to make target 'geode'` | old target name | targets are `linux`, `windows`, `solaris`; everything else is a flag |
| backends print different text | a kernel is broken | compare against `--backend ref` |

---

## Solaris / SPARC

Separate world. Different compiler, big-endian, 64-bit, no MMX.

```sh
gmake solaris              # Sun Studio, 64-bit
gmake solaris PROFILE=1      # ...with stage timers
gmake solaris TOOLCHAIN=gcc          # GCC on Solaris, 64-bit
gmake solaris-test         # test programs
```

Three things matter and all three are already handled by the targets:

1. `-lsocket -lnsl` — Solaris keeps the socket API outside libc.
2. Sun Studio option spellings — `-Xc -xc99=none -xO5 -xtarget=`, not
   GCC's.
3. **Build 64-bit; do not pass `-D_FILE_OFFSET_BITS=64`.** Under `-Xc`
   there is no `long long`, so `<sys/types.h>` makes `off_t` an opaque
   union that cannot be passed to `mmap()`. In LP64 `off_t` is a plain
   `long` and the problem vanishes.

Full explanation in [`../COMPILE.md`](../COMPILE.md) section 6.
