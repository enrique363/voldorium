#!/usr/bin/env bash
# Every main menu entry: boots to the main menu (M1_BACK), picks the entry (M1_MENU_PICK) and taps
# A (and Start now and then) to get into the mode, with an idle player 2 (M1_PORT_B) for VS BATTLE.
# Fails on a stop other than the time limit (such as a call to code that was not translated).
# Usage: bash tests/modes.sh [seconds per mode, default 60] [entries, default all but 2 and 11]
source "$(dirname "$0")/common.sh"
SECS=${1:-60}
ENTRIES=${2:-"0 1 3 4 5 6 7 8 9 10 12"}
pads() { local s=""; for i in $(seq 0 200); do f=$((900+i*45)); b=a; [ $((i % 7)) = 6 ] && b=s; s="$s,$f:$b,$((f+5)):-"; done; echo "${s#,}"; }
P=$(pads)
fail=0
run() {
  local e=$1 d="$OUT/modes/$1"
  rm -rf "${OUT:?}/modes/$1"; mkdir -p "$d"
  ( cd "$d" && M1_PORT_B=1 M1_BACK=- M1_MENU_PICK=0:$e M1_LOBBY_SERVER=none M1_FULLSCREEN=0 M1_NOSOUND=1 M1_FAST=1 M1_PADS="$P" \
      M1_SHOTS=1500,3000,6000 M1_VMU="$(cygpath -m "$d")/vmu_a1.bin" timeout $((SECS + 60)) "$EXE" "$SCW" $SECS > log.txt 2>&1 )
}
for e in $ENTRIES; do run $e & while [ "$(jobs -r | wc -l)" -ge 4 ]; do sleep 1; done; done
wait
for e in $ENTRIES; do
  d="$OUT/modes/$e"
  stop=$(grep -m1 "=== M1 stop" "$d/log.txt" | sed 's/=== M1 stop: //')
  modes=$(grep -o "mode [0-9]*" "$d/log.txt" | sort -u | tr '\n' ' ')
  echo "entry $e: $stop"
  case "$stop" in "watchdog: time limit reached") ;; *) fail=1 ;; esac
  for n in 1500 3000 6000; do [ -f "$d/shot_$n.ppm" ] && ppm2png "$d/shot_$n.ppm" "$d/shot_$n.png" && rm "$d/shot_$n.ppm"; done
done
[ $fail = 0 ] && echo "PASS: every mode ran" || echo "FAIL: a mode stopped"
