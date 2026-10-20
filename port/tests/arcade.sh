#!/usr/bin/env bash
# Arcade mode start to finish: player 1 keeps full life and player 2 a sliver in every fight
# (M1_TEST_P2KO), A is tapped throughout, unpaced. Fails when the game freezes (no frame for 8 s:
# M1_HANG_EXIT, the hang report is in the log) or stops for another reason than the time limit.
# Usage: bash tests/arcade.sh [game seconds, default 2600] (about 40 minutes)
source "$(dirname "$0")/common.sh"
SECS=${1:-2600}
D="$OUT/arcade"; mkdir -p "$D"; rm -f "$D/vmu_a1.bin"
P="$PADS"; f=3000; for i in $(seq 1 3000); do P="$P,$f:a,$((f+4)):-"; f=$((f+60)); done
( cd "$D" && M1_HANG_EXIT=1 M1_TEST_P2KO=1 M1_FAST=1 M1_NOSOUND=1 M1_FULLSCREEN=0 M1_PADS="$P" \
    M1_VMU="$(cygpath -m "$D")/vmu_a1.bin" timeout $((SECS + 300)) "$EXE" "$SCW" $SECS > log.txt 2>&1 )
r=$?
echo "fights: $(grep -c 'test: mode 1 ' "$D/log.txt")"
if grep -q "=== hang" "$D/log.txt"; then echo "FAIL: the game froze"; grep -A6 "=== hang" "$D/log.txt"
elif grep -q "M1 stop: watchdog: time limit reached" "$D/log.txt"; then echo "PASS: no freeze"
else echo "FAIL: stopped ($r)"; grep "M1 stop" "$D/log.txt"; fi
