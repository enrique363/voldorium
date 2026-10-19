#!/usr/bin/env bash
# Builds the broad-oracle replay harness (WSL): every translated function + dispatch table + replay.c.
# usage: bash m0/build_replay.sh   (from sc/port; run sh4c/sweep.py first)
set -euo pipefail
cd "$(dirname "$0")/.."
O=/tmp/replay_obj; rm -rf $O; mkdir -p $O
for f in build/sweep/all_*.c build/sweep/dispatch.c; do
  gcc -O1 -ffp-contract=off -fno-strict-aliasing -c -I sh4c -o $O/$(basename $f .c).o $f &
done
gcc -O1 -ffp-contract=off -c -I sh4c -I ~/sc-flycast-m0/core/hw/sh4 -o $O/fsca.o sh4c/fsca.c &
gcc -O1 -ffp-contract=off -Wall -c -I sh4c -o $O/replay.o m0/replay.c &
wait
gcc -o /tmp/replay $O/*.o -lm
echo built /tmp/replay
