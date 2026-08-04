#!/bin/bash
# Count dynamic instructions in the VIS kernels, compiled for the real
# SPARC64 target with the repo's own -mcpu=ultrasparc -mvis flags.
#
# The kernels are marked VIS_NOINLINE so they survive as functions; the
# fixed-trip inner loops are fully unrolled. The numbers are GCC's --
# Sun Studio's scheduling differs, but the kernel structure is the
# same under both compilers.
#
# Usage: cross-count.sh [OPTLEVEL]
#   OPTLEVEL defaults to -O3; -O2 matches the Makefile's gcc-vis build.
set -e
OPT="${1:--O3}"
CROSS="${CROSS:-sparc64-linux-gnu-gcc}"
STUBS="${VIS_STUBS:-}"
SRC="$(cd "$(dirname "$0")/.." && pwd)/src"
OUT=/tmp/viskern
mkdir -p $OUT

EXTRA=()
if [ -n "$STUBS" ]; then
    EXTRA=(-nostdinc -I"$STUBS")
fi

$CROSS -std=gnu89 $OPT -funroll-loops -DINFER_VIS_COUNT \
    --param max-completely-peeled-insns=4000 --param max-completely-peel-times=20 \
    -mcpu=ultrasparc -mvis \
    -DINFER_HAVE_VIS -DINFER_BIG_ENDIAN=1 "${EXTRA[@]}" -I"$SRC" \
    -S -o $OUT/backend_vis.s "$SRC/backend_vis.c"

echo "== VIS instructions actually emitted =="
grep -oE "\b(fmul8x16|fpadd16|fpadd32|fmuld8ulx16|fzero)\b" $OUT/backend_vis.s \
    | sort | uniq -c

echo
echo "== per-kernel dynamic counts (ncols = 1024) =="
python3 "$(dirname "$0")/count_dynamic.py" $OUT/backend_vis.s
