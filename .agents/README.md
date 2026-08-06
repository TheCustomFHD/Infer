# .agents/

Guidance for AI coding agents working on this repository.

**Start with [`../AGENTS.md`](../AGENTS.md)** at the repository root —
that is the short version and it is where most harnesses look. These
files are the detail behind it.

| file | when to read it |
|---|---|
| [`BUILDING.md`](BUILDING.md) | before any build, and whenever one fails. The exact Windows XP recipe with expected output. |
| [`CONVENTIONS.md`](CONVENTIONS.md) | before writing C. The rules are stricter than they look. |
| [`PITFALLS.md`](PITFALLS.md) | before debugging anything, and before optimising anything. |

## The 30-second version

```sh
sudo apt-get install -y build-essential gcc-multilib \
                        gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686
make all          # 4 binaries: linux, linux64, windows.exe, windows64.exe
make test && ./build/t_align && ./build/t_agent && ./build/t_endian
make i8-split-test && ./build/t_ident && ./build/t_thread && ./build/t_avx2acc
```

Three things that trip agents up more than anything else:

1. **ANSI C (C89) only**, no dependencies. No `//`, no `long long`, no
   declarations after statements. One exception: `src/backend_mmx.c`.
2. **There are four x86 artifacts, not twelve.** Each contains all
   four backends — `avx2`, `mmx`, `i8`, `ref` — and uses an i486
   baseline. `backend_avx2.c` is compiled separately with `-mavx2`
   and gated at run time; nothing else may contain SIMD. Do not add
   artifacts or per-ISA downloads.
3. **The bugs here do not crash.** They return confident nonsense. Run
   the verification commands; do not assume a clean compile means a
   working binary. Two that shipped: an AVX2 kernel registered on the
   wrong macro so it was never selected (finding 50), and `static`
   scratch in the MMX kernels that raced under threading and emitted
   fluent gibberish while every single-threaded test passed
   (finding 52).
4. **Threading is opt-in** (`--threads`, default 1) and splits rows,
   never dot products. No kernel may hold per-call scratch in a
   `static`.

## Also worth reading

- [`../COMPILE.md`](../COMPILE.md) — every target and flag, with
  expected output. Section 2 is a step-by-step Windows XP recipe.
- [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) — how the pieces
  fit; read before any structural change.
- [`../docs/FINDINGS.md`](../docs/FINDINGS.md) — 52 measured surprises.
- [`../docs/PERFORMANCE-ANALYSIS.md`](../docs/PERFORMANCE-ANALYSIS.md) —
  read before optimising. Contains every rejected approach and why.
