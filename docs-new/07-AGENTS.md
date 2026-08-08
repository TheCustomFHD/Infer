# Working on infer as an agent

If you read one thing, read [the loop](#the-loop) and
[the hard rules](#the-hard-rules).

- [The hard rules](#the-hard-rules)
- [The loop](#the-loop)
- [Session setup](#session-setup)
- [Session teardown](#session-teardown)
- [Shipping a release](#shipping-a-release)
- [Code conventions](#code-conventions)
- [How to report results](#how-to-report-results)

---

## The hard rules

Break these and something ships broken.

| rule | why |
|---|---|
| **ANSI C (C89) only** | exceptions: `backend_mmx.c` (GNU asm, `-std=gnu89`), `backend_avx2.c` (intrinsics, `-mavx2`), `backend_vis.c` (SPARC) |
| **Never name a 64-bit integer type** | `long long` / `int64_t` broke Solaris **twice**. Use `long` or two 32-bit halves |
| **No third-party content in the repo** | no model weights, no chat templates |
| **No personal or location data anywhere** | including in comments and commit messages |
| **Four artifacts, no more** | a new kernel joins the runtime table as a gated object; it does **not** become a build target |
| **Threading is opt-in**, default 1 | `-T 0` means one per core |
| **`git add -A`, never `git add *`** | the glob skips dotfiles and once dropped all of `.github/` |
| **Bump the version every time** | and the changelog head **must** match, or the release guard fails |
| **Delete the model at the end of a turn** | 508 MiB; the workspace prunes large files |
| **Run the full CI suite locally** | extract every step from `build.yml`; spot-checking let two bugs ship |

## The loop

```
 1. TEST AND RECORD          baseline, written down, machine idle
 2. SEARCH FOR MORE TESTS    what else can I measure?
 3. RUN THOSE TESTS          now you know WHERE it hurts
 4. SEARCH FOR SOLUTIONS     how have others fixed THIS?
 5. IMPLEMENT                smallest change that tests the idea
 6. TEST AGAIN               full model, interleaved, medians
 7. REVERT WHEN NEEDED       no measured benefit => it does not ship
 8. REPEAT                   until an explicit goal is reached
```

**The two search steps are different, and the order matters.** Step 2
looks for *diagnostics* — you do not yet know what is slow. Step 4
looks for *fixes*, and only makes sense after step 3 has told you what
to fix. Searching for solutions first is how you implement a
cache-locality fix for what turns out to be a scale-decode problem.

Two techniques that repeatedly found what nothing else did:

- **Time by subtraction** — strip the kernel, add one layer at a time.
  Found that 73% of the Q4_K kernel was neither loads nor arithmetic.
- **Normalise by work** — `us/call` compares tensor shapes, not kernel
  quality. That mistake drove an entire release.

Expect to be wrong. Hugepages, L3 eviction, prefetch overshoot and the
Win32 `SetEvent` storm were all plausible and all measured wrong.
Check [the findings index](reference/findings.md) before starting.

Full version with the measurements:
[04-PERFORMANCE.md](04-PERFORMANCE.md#the-optimisation-loop).

## Session setup

The sandbox resets between sessions and drops toolchains, the model,
and anything outside the snapshot.

```sh
# toolchains
sudo apt-get install -y -qq \
  gcc-multilib \
  gcc-mingw-w64-i686-win32 binutils-mingw-w64-i686 \
  gcc-mingw-w64-x86-64-win32 binutils-mingw-w64-x86-64 \
  gcc-sparc64-linux-gnu libc6-dev-sparc64-cross \
  gcc-s390x-linux-gnu qemu-user-static

# optional: Windows execution
sudo apt-get install -y -qq wine
export WINEPREFIX="$HOME/.cache/wineprefix"   # NOT $HOME directly: 1.4 GB
export WINEDEBUG=-all

# model
mkdir -p ~/models && curl -sL \
  "https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf" \
  -o ~/models/Qwen3.5-0.8B-Q4_K_M.gguf

# sanity
make all && make i8-split-test && ./build/t_ident && ./build/t_thread
```

**Back up before large changes.** The workspace can evict files:

```sh
mkdir -p ~/backups
(cd ~ && zip -rq "backups/infer-BACKUP-$(date +%F-%H%M%S).zip" \
   Infer-* -x '*/build/*' '*.o')
```

Verify the backup restores before relying on it.

## Session teardown

```sh
rm -f ~/models/*.gguf            # 508 MiB
rm -rf ~/.cache/wineprefix       # 1.4 GB
pkill -9 wineserver 2>/dev/null  # stragglers eat a core
make clean
git status --porcelain           # review before committing
```

Full checklist: [reference/workspace.md](reference/workspace.md).

Check `df -h ~` if anything felt slow — a full workspace evicts files
mid-run and produces baffling failures.

## Shipping a release

```sh
# 1. bump BOTH, they are asserted to agree
#      src/infer.h   #define INFER_VERSION "X.Y.Z"
#      Makefile      VERSION = X.Y.Z
make checkversion

# 2. changelog head MUST name the same version
head -3 CHANGELOG.md

# 3. build and verify everything
make clean && make all
make test && make i8-split-test
# ...then the full local CI extraction, see 05-TESTING.md

# 4. commit BEFORE tagging (git archive packages the commit)
git add -A
git commit -m "X.Y.Z"
git tag vX.Y.Z
git push origin main
git push origin vX.Y.Z          # ONE tag; never --tags with a backlog
```

**Push tags one at a time.** More than three at once and GitHub
creates **no events at all** — the workflow never runs, and it looks
identical to a broken workflow.

Then **watch the Actions tab**. Every guard assumes a run happens.

## Code conventions

- **Comments explain *why*, and cite measurements.** The codebase's
  comments are load-bearing documentation; several encode findings that
  would otherwise be re-litigated. Keep that standard.
- **One option table.** Add a flag as one row in `opts.c`; parsing,
  help and validation follow.
- **One prompt builder.** `agent.c` serves all three modes. Do not
  special-case a mode.
- **Kernel scratch is automatic, never `static`.**
- **No allocation in the hot path.**
- **Guard properties no test can see.** If a bug is invisible to unit
  tests (a source-level shape, a build property), add a CI grep — and
  **negative-control it**.

## How to report results

The user has repeatedly corrected over-claiming, and values negative
results backed by evidence. Match that.

**Do:**

- State the platform, binary, thread count and token count.
- Quote medians of interleaved runs, with the spread.
- Say what you did not test.
- Report a reverted change and why.

**Do not:**

- Quote a best-of-N as if it were typical.
- Compare across platforms without saying so.
- Claim "no regression" when the honest summary is "four releases
  delivered nothing".
- Present a static inspection as if it were an execution.

A worked example of the right shape:

> `-T 2`: 17.25 → 22.16 tok/s (+28.5%), medians of 5 interleaved runs,
> sandbox Xeon 2 cores, **Linux** `infer-linux64`, 40 tokens greedy.
> `-T 1` unchanged (13.99 → 14.12, noise — the pool is not used).
> Not tested on real Windows; Wine numbers are ~16% lower at one
> thread from Wine overhead alone.
