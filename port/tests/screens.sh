#!/usr/bin/env bash
# Screenshot set (16:9): title menu, character select, fight; PNGs in build/tests.
# Usage: bash tests/screens.sh
source "$(dirname "$0")/common.sh"
cd "$OUT"
rm -f shot_*.ppm
M1_FAST=1 M1_NOSOUND=1 M1_SHOTS=1200,2000,5600 M1_PADS="$PADS" "$EXE" "$SCW" 60 > screens_wide.txt 2>&1
for n in 1200 2000 5600; do [ -f shot_$n.ppm ] && ppm2png shot_$n.ppm screen_wide_$n.png; done
ls screen_*.png
