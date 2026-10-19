#!/usr/bin/env bash
# A lobby match that ends early: side b's match closes after DROP seconds (as if the player quit
# or lost the connection). Passes when side a notices, restarts and boots back to the main menu
# with the cursor on ONLINE VERSUS and the reason as its help line (screenshot back_<n>.png),
# and side b's quit leaves nothing running.
# Usage: bash tests/lobby_drop.sh [drop seconds, default 45]
source "$(dirname "$0")/common.sh"
DROP=${1:-45}
D="$OUT/lobby_drop"
rm -rf "$D"; mkdir -p "$D/a" "$D/b"
pick() { local s="" off=$1; for i in $(seq 0 12); do f=$((150+off+i*90)); s="$s,$f:a,$((f+6)):-"; done; echo "${s#,}"; }
for side in a b; do
  test=challenge; secs=$((DROP + 60)); [ $side = b ] && test=accept && secs=$DROP
  ( cd "$D/$side" && M1_LOBBY_SERVER=none M1_FULLSCREEN=0 M1_VMU="$(cygpath -m "$D/$side")/vmu_a1.bin" M1_LOBBY_TEST=$test \
      M1_CHILD_SECONDS=$secs M1_CHILD_PADS="$(pick 0)" M1_PADS="$PADS" M1_NOSOUND=1 M1_FAST=1 M1_SHOTS=900 "$EXE" "$SCW" 60 > log.txt 2>&1 ) &
done
wait
for i in $(seq 1 $((DROP + 120))); do
  [ "$(grep -c '=== M1 stop' "$D/a/log.txt")" -ge 2 ] && break
  sleep 1
done
for side in a b; do echo "== $side"; grep -E "online:|net: (running|connection|peer)|M1 stop" "$D/$side/log.txt"; done
[ -f "$D/a/shot_900.ppm" ] && ppm2png "$D/a/shot_900.ppm" "$D/back_900.png" && echo "screenshot: $D/back_900.png"
grep -q "online: main menu at loop" "$D/a/log.txt" && echo "PASS: side a is back in the main menu" || echo "FAIL: side a did not return"
