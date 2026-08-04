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
infer-1.14.0-linux
infer-1.14.0-linux-profile
infer-1.14.0-windows-profile.exe
infer-1.14.0-windows.exe
```

Four binaries, no more. There is no separate MMX or i486 build: all four
contain all three backends and use an i486 baseline.

Individually:

```sh
make linux
make linux-profile
make windows
make windows-profile
```

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
  mmx   ok  integer kernels with MMX inner loop (Pentium MMX / K6 / Geode+)   <- selected
  i8    ok  integer kernels, portable C (recommended)
  ref   ok  scalar float reference, maximum portability (486-safe)
```

---

## 5. Test

```sh
make test
./build/t_align      # MMX constant alignment -- guards a real fault
./build/t_agent      # cross-mode prompts, tool-call shapes
./build/t_endian     # byte-order neutrality
./build/t_jinja      # template engine
```

Each ends in a line saying it passed.

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
| `--log-stages` prints nothing | non-profile build | use `make linux-profile` |
| `make: *** No rule to make target 'geode'` | old target name | targets are `linux`, `linux-profile`, `windows`, `windows-profile` |
| backends print different text | a kernel is broken | compare against `--backend ref` |

---

## Solaris / SPARC

Separate world. Different compiler, big-endian, 64-bit, no MMX.

```sh
gmake solaris              # Sun Studio, 64-bit
gmake solaris-profile      # ...with stage timers
gmake solaris-gcc          # GCC on Solaris, 64-bit
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
