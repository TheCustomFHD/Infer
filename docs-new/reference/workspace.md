# Workspace and cleanup

The sandbox this project is developed in has a **finite budget** and
prunes large files without warning. Several confusing failures have
turned out to be eviction, not bugs. This page is the checklist.

## What eats the budget

| thing | size | persists? | notes |
|---|---|---|---|
| the model `.gguf` | **508 MiB** | yes, if under `~` | evicted when the workspace is over budget |
| a Wine prefix | **1.4 GB** | only if outside `.cache` | the biggest single offender |
| `build/` | ~2 MB | **no** (excluded) | safe |
| release zips in `local releases/` | ~13 MB total | yes | archive old ones |
| backups | ~1.4 MB each | yes | keep a couple, not twenty |
| cross-toolchains (apt) | ~500 MB | **no** | reinstalled every session |

Directory names **excluded from snapshots** (safe to fill):
`.cache`, `.venv`, `build`, `dist`, `out`, `target`, `node_modules`,
`__pycache__`, `.pytest_cache`, and others.

## Standard cleanup

Run at the end of every working session:

```sh
rm -f  ~/models/*.gguf              # 508 MiB, re-downloads in ~15 s
rm -rf ~/.cache/wineprefix          # 1.4 GB
pkill -9 wineserver 2>/dev/null     # stragglers eat a whole core
make clean                          # build artifacts
df -h ~                             # confirm
```

## Wine specifically

Wine caused the worst workspace incident on this project: a 1.4 GB
prefix created under `$HOME` pushed the workspace over budget, the
model was evicted **mid-benchmark**, and the resulting empty output
looked exactly like a threading deadlock. Two hours went into
diagnosing a bug that did not exist.

Three rules:

```sh
export WINEPREFIX="$HOME/.cache/wineprefix"   # excluded from snapshots
export WINEDEBUG=-all                         # or the log is unreadable
pkill -9 wineserver                           # between every benchmark run
```

**`WINEPREFIX` must be a directory you own** — `/tmp` fails with
`wine: '/tmp' is not owned by you`.

**A leftover Wine process is not idle.** One from an aborted benchmark
sat at 97% CPU for 25 minutes and silently halved every measurement
taken in that window, including a Linux run that made threading look
like a regression. Always:

```sh
uptime      # load should be ~0 before you trust a measurement
```

## Backups

Before any large refactor:

```sh
mkdir -p ~/backups
cd ~ && zip -rq "backups/infer-BACKUP-$(date +%Y%m%d-%H%M%S).zip" \
        Infer-* -x '*/build/*' '*.o'
```

**Verify it restores**, do not assume:

```sh
cd /tmp && rm -rf vb && mkdir vb && cd vb
unzip -q ~/backups/infer-BACKUP-*.zip
cd Infer-* && make version && git log --oneline | head -2
cd /tmp && rm -rf vb
```

Keep the `.git` directory in the backup — it is small and it is the
only history that survives an eviction.

## Archiving old releases

`local releases/` accumulates. Fold old versions into one archive
rather than deleting them:

```sh
cd ~/"local releases"
mkdir -p /tmp/old && mv infer-1.1[5-9]*.zip infer-1.2[01]*.zip /tmp/old/
(cd /tmp/old && zip -9 -rq ~/"local releases/ARCHIVE-old-releases.zip" .)
rm -rf /tmp/old
```

## Symptoms of an over-budget workspace

| symptom | actual cause |
|---|---|
| "model file not found" mid-session | evicted |
| a benchmark suddenly returns empty output | model evicted between runs |
| toolchains missing that you installed an hour ago | not persisted; reinstall |
| `.wineprefix` recreated itself | the whole directory was pruned |
| numbers half what they were | a leftover process, not eviction — check `uptime` |

When something inexplicable happens, check `df -h ~` and
`ls ~/models/` **before** debugging the code.
