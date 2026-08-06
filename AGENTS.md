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
make linux              # Linux/x86     -> infer-<version>-linux
make windows            # Windows/x86   -> infer-<version>-windows.exe
make solaris            # Solaris/SPARC -> infer-<version>-solaris
make all                # all four shipping artifacts
make help               # targets and flags
make test               # unit tests into build/
```

**There are exactly three build targets.** Everything else is a flag:

| flag | effect |
|---|---|
| `PROFILE=1` | per-stage timers (every shipped build has them) |
| `NATIVE=1` | `-march=native`; runs only on the build machine |
| `BITS=64` | 64-bit build (default 32) |
| `TOOLCHAIN=gcc` | Solaris via GCC instead of Sun Studio |
| `NO_MMX=1` `NO_AVX2=1` `NO_VIS=1` | leave a kernel out |
| `NO_THREADS=1` | compile the thread pool away |
| `NO_SSE2=1` | reserved; hand-written SSE2 kernel not yet written |

Every binary contains **every kernel it could use** and picks one at run
time after a CPUID probe (plus `XGETBV` for AVX2): `avx2`, `mmx`, `i8`,
`ref` on x86; `vis`, `i8`, `ref` on SPARC. The x86 builds use an i486
baseline, so one binary runs on a 486 and accelerates on anything newer.

**`backend_avx2.c` is the only object compiled with `-mavx2`**, in a
separate stage, linked against everything else built at the baseline.
Compiling it together with the rest would apply `-mavx2` project-wide
and the binary would fault on a 486 before `main()`. Two consequences
for anyone editing this:

- The backend table registers `avx2` on **`INFER_HAVE_AVX2`**, never
  on `__AVX2__` — the latter is false in `backend.c`, so testing it
  silently drops the kernel from every shipped build. (Finding 50.)
- No intrinsics in `backend.c` or `qwen35.c`. Anything AVX2 goes
  behind a `bk_a2_*` entry point in `backend_avx2.c` that returns 0
  when unavailable, with a scalar fallback at the call site.

**Threading is opt-in** (`--threads`, default 1) and rows are split,
never dot products, so results are bit-identical at any thread count.
Kernels must not keep per-call scratch in a `static` — that is a data
race that produces plausible wrong output rather than a crash, and it
shipped once. (Finding 52.)

**Do not add targets, and do not add artifacts.** No "MMX build", no
"486 build", no "`-profile` target", and no per-ISA download. 1.18-1.20
shipped a `-sse2`/`-avx2` ladder and it was removed in 1.21.0: it made
the user diagnose their own CPU, and a wrong guess died with an illegal
instruction instead of running slower. A new kernel joins the runtime
table as a gated object; it does not become a build flag or a file to
download.

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

## Releases

CI builds and verifies on every push. A `v*` tag additionally publishes
a GitHub Release using the binaries that build already verified.

```sh
# bump BOTH, they must agree
#   src/infer.h   #define INFER_VERSION "X.Y.Z"
#   Makefile      VERSION = X.Y.Z
make checkversion

git commit -am "1.12.2"     # commit BEFORE tagging: git archive packages the commit
git tag v1.12.2
git push && git push --tags
```

Do not tag before committing the version bump — the workflow checks for
it and fails, but it wastes a run.

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
- [`docs/FINDINGS.md`](docs/FINDINGS.md) — 55 measured surprises
