# AGENTS.md

Entry point for AI coding agents. **Read this file completely before
your first edit.** It is short on purpose; everything else is one
click away.

---

## The 10 rules that break things if ignored

| # | rule |
|---|---|
| 1 | **ANSI C (C89)**. Exceptions: `backend_mmx.c` (GNU asm), `backend_avx2.c` (intrinsics), `backend_vis.c` (SPARC) |
| 2 | **Never name a 64-bit integer type.** `long long`/`int64_t` broke Solaris twice |
| 3 | **No third-party content.** No weights, no chat templates, no vendored code |
| 4 | **No personal or location data**, including in comments |
| 5 | **Four artifacts only.** A new kernel is a gated object, not a build target |
| 6 | **Threading is opt-in**, default 1 thread |
| 7 | **`git add -A`**, never `git add *` (the glob skips dotfiles) |
| 8 | **Bump the version every change**; the changelog head must match |
| 9 | **Delete the model** (508 MiB) and the Wine prefix (1.4 GB) when done |
| 10 | **Run the full CI suite locally** before claiming done |

## Start here

```sh
# setup (the sandbox resets and drops all of this)
sudo apt-get install -y -qq gcc-multilib \
  gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686 \
  gcc-mingw-w64-x86-64-win32 binutils-mingw-w64-x86-64 \
  gcc-sparc64-linux-gnu libc6-dev-sparc64-cross \
  gcc-s390x-linux-gnu qemu-user-static

mkdir -p ~/models && curl -sL \
 "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf" \
 -o ~/models/Qwen3.5-0.8B-Q4_K_M.gguf

# prove the tree is healthy
make all && make i8-split-test && ./build/t_ident && ./build/t_thread
```

## Where everything is

| you want to | read |
|---|---|
| understand the project | [docs-new/README.md](docs-new/README.md) |
| use the binary | [docs-new/01-USING.md](docs-new/01-USING.md) |
| build it, any target | [docs-new/02-BUILDING.md](docs-new/02-BUILDING.md) |
| understand the code | [docs-new/03-ARCHITECTURE.md](docs-new/03-ARCHITECTURE.md) |
| **make it faster** | [docs-new/04-PERFORMANCE.md](docs-new/04-PERFORMANCE.md) — includes the optimisation loop |
| test it | [docs-new/05-TESTING.md](docs-new/05-TESTING.md) |
| **avoid a known trap** | [docs-new/06-PITFALLS.md](docs-new/06-PITFALLS.md) — searchable by symptom |
| **work effectively here** | [docs-new/07-AGENTS.md](docs-new/07-AGENTS.md) — the full workflow |
| look up a flag | [docs-new/reference/cli.md](docs-new/reference/cli.md) |
| find the right source file | [docs-new/reference/files.md](docs-new/reference/files.md) |
| check if an idea was tried | [docs-new/reference/findings.md](docs-new/reference/findings.md) |
| clean up / avoid eviction | [docs-new/reference/workspace.md](docs-new/reference/workspace.md) |

## The loop, in one line

**Test and record → search for more tests → run them → search for
solutions → implement → test again → revert if it does not pay →
repeat until the goal is reached.**

The two search steps are deliberate and ordered: find *diagnostics*
first, *fixes* only once you know what is broken.

Most confident hypotheses on this project were wrong (hugepages, L3
eviction, prefetch overshoot, the Win32 `SetEvent` storm). Check
[the findings index](docs-new/reference/findings.md) before starting:
your idea may already have been measured and rejected.

Detail: [docs-new/04-PERFORMANCE.md](docs-new/04-PERFORMANCE.md#the-optimisation-loop).

## Before you say you are done

```sh
make clean && make all              # 0 warnings
make test && make i8-split-test     # all pass
# full CI extraction — see docs-new/05-TESTING.md
rm -f ~/models/*.gguf               # tidy up
rm -rf ~/.cache/wineprefix
pkill -9 wineserver 2>/dev/null
git status --porcelain              # review
```

## Reporting

State the platform, binary, thread count and token count. Quote
**medians of interleaved runs**, not best-of-N. Say what you did not
test. Report reverted changes and why.

The user values negative results backed by evidence and has corrected
over-claiming repeatedly. A correct "this did not work, here is the
measurement" is worth more than an optimistic number.
