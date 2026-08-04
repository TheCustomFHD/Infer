# AGENTS.md

Instructions for AI coding agents working on this repository.

This file is at the repository root because that is where most agent
harnesses look for it. The same content, plus per-task detail, is in
[`.agents/`](.agents/).

**If you read nothing else, read [Build it](#build-it) and
[Never do these](#never-do-these).**

---

## What this is

`infer` is a GGUF inference server for the Qwen3.5 architecture, written
in **ANSI C (C89)** with **no third-party dependencies** — only libc,
libm and the OS socket API. It targets old hardware: i486, Windows XP,
Geode LX, UltraSPARC.

Those constraints are the point of the project, not an accident. Most
mistakes agents make here come from applying modern-toolchain habits.

---

## Build it

```sh
make linux              # Linux/x86        -> infer-<version>-linux
make linux-profile      # ...with profiler
make windows            # Windows XP/x86   -> infer-<version>-windows.exe
make windows-profile    # ...with profiler
make all                # all four
make help               # the list
make test               # unit tests into build/
```

There are **only four x86 targets**. Every one contains all three
compute backends (`mmx`, `i8`, `ref`) and picks the fastest at run time.
Every one uses an i486 baseline. Do not add "an MMX build" or "a 486
build" — one binary already covers both.

Solaris/SPARC is separate: `make solaris`, `make solaris-gcc`. Different
compiler, different flags, big-endian, 64-bit.

### First, in a fresh sandbox

```sh
sudo apt-get update
sudo apt-get install -y build-essential gcc-multilib \
                        gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686
```

Sandboxes commonly discard installed packages between turns. If
`i686-w64-mingw32-gcc: command not found` appears halfway through a
session, reinstall and carry on — nothing is broken.

`binutils-mingw-w64-i686` is easy to forget and provides
`i686-w64-mingw32-objdump`, which every Windows verification step needs.

---

## Verify it

"It compiled" is not "it works". These are the checks that matter, and
CI runs all of them:

```sh
V=$(make version)

# no CMOV -- a Pentium Pro instruction that faults on a 486
objdump -d infer-$V-linux --no-show-raw-insn \
  | grep -cE '^[[:space:]]+[0-9a-f]+:[[:space:]]+cmov[a-z]+'      # want 0

# the MMX kernels really are compiled in
objdump -d infer-$V-linux | grep -cE 'pmaddwd|punpcklbw'          # want >100

# exactly three DLLs, all present on a stock XP install
i686-w64-mingw32-objdump -p infer-$V-windows.exe | grep -c 'DLL Name'   # want 3

# all three backends, and which one this CPU gets
./infer-$V-linux --backend list

# the tests
make test && ./build/t_align && ./build/t_agent && ./build/t_endian
```

### Correctness: make the backends disagree

The strongest cheap test. `-t 0` is greedy, so all three **must** print
identical text:

```sh
for b in mmx i8 ref; do
  ./infer-$V-linux run model.gguf -p "The capital of France is" \
      -n 8 -t 0 --seed 1 --backend $b -c 256
done
```

If they differ, a kernel is broken.

---

## Never do these

**Do not add a dependency.** Not a library, not a header-only anything,
not a build system. libc, libm, sockets. That is the whole list.

**Do not write C99.** Declarations at the top of blocks, `/* */`
comments, no `//`, no `long long`, no VLAs, no designated initialisers.
`src/backend_mmx.c` is the sole exception (GNU inline asm, built with
`-std=gnu89`), and it is x86-only.

**Do not name a 64-bit integer type.** No `long long`, no `int64_t`.
C90 has neither, and Solaris under `-Xc` turns `off_t` into an opaque
union you cannot even cast to. Large offsets are carried as `double`
(`gg_off`), exact to 2^53. This has broken real builds twice.

**Do not cast a byte pointer to a wider type** to read model data. The
blob is only byte-aligned, and the file is little-endian regardless of
host. Use `inf_rd_f32p`, `inf_rd_f64p`, `rd_f16p`, `inf_rd_f32v`.

**Do not guess byte order.** If detection cannot tell, the build must
fail. A wrong guess produces a program that loads the model, prints
correct metadata, runs at full speed and emits garbage. See finding 20.

**Do not trust a microbenchmark.** This project has a graveyard of
optimisations that measured 2.46× in isolation and 0.97× in reality.
See `docs/PERFORMANCE-ANALYSIS.md` before optimising anything.

**Do not bundle third-party content.** No model weights, no chat
templates. Link to the source instead.

**Do not try to run the Windows `.exe` under Wine** in a sandbox. Debian
Wine fails its own prefix bootstrap. Verify statically and test the
Linux build, which is the same sources with two files swapped.

**Do not leave a 500 MB model in the workspace.** Delete it when you
finish. Sandboxes prune large workspaces, sometimes taking source with
them.

---

## Where things live

| you want to change | edit |
|---|---|
| a command-line option | one row in `src/opts.c`'s table — parsing, `--help` and validation all follow |
| prompt building or tool calls | `src/agent.c`, once, for all three modes |
| an API endpoint | `src/server.c` routing |
| a chat command | `src/chat.c` |
| the template engine | `src/jinja_eval.c`, then re-verify byte-for-byte against Python |
| a compute kernel | `src/backend.c` (i8), `src/backend_mmx.c` (mmx), `src/quant.c` (ref) |
| the model architecture | `src/qwen35.c` |
| OS-specific code | `src/net_*.c`, `src/sys_*.c` — **only these four files know an OS exists** |

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before a structural
change. It has a full request walkthrough.

---

## Before you say you are done

```sh
make clean && make all          # zero warnings, or it is a bug
make test && ./build/t_align && ./build/t_agent && ./build/t_endian
make checkversion               # Makefile VERSION == src/infer.h
rm -f *.gguf                    # do not leave the model behind
```

If you changed anything user-visible, update `CHANGELOG.md`. If you
changed a build flag, update `COMPILE.md` — and **run the command you
wrote** to confirm it produces the output you claimed.

---

## More detail

- [`.agents/BUILDING.md`](.agents/BUILDING.md) — the exact Windows XP
  recipe, step by step, with expected output
- [`.agents/CONVENTIONS.md`](.agents/CONVENTIONS.md) — C89 rules and
  house style
- [`.agents/PITFALLS.md`](.agents/PITFALLS.md) — bugs that have actually
  happened here, and how they were found
- [`COMPILE.md`](COMPILE.md) — every target, every flag
- [`docs/FINDINGS.md`](docs/FINDINGS.md) — 20 measured surprises
