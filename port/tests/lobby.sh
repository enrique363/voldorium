#!/usr/bin/env bash
# Online Versus lobby, end to end on one PC: two copies open the in-game lobby (M1_LOBBY_TEST),
# one challenges the first player it finds, the other accepts; both restart into the match
# (scripted boot into VS BATTLE character select, then GGPO) and play the M1_PADS script. Each
# copy has its own memory card in the lobby, as players do; the matches run without one.
# Passes when both boots end at the same loop and the netplay checksums match.
# Usage: bash tests/lobby.sh [match seconds, default 90]
source "$(dirname "$0")/common.sh"
SECS=${1:-90}
# the match: both pick in VS character select (A taps from netplay frame 150), then mash in the fight
mash() { local s="" off=$1; for i in $(seq 0 150); do f=$((1500+off+i*20)); case $(((i+off)%5)) in 0) b=a;; 1) b=b;; 2) b=x;; 3) b=u;; 4) b=d;; esac; s="$s,$f:$b,$((f+4)):-"; done; echo "${s#,}"; }
pick() { local s="" off=$1; for i in $(seq 0 12); do f=$((150+off+i*90)); s="$s,$f:a,$((f+6)):-"; done; echo "${s#,}"; }
mkdir -p "$OUT/lobby_a" "$OUT/lobby_b"
for side in a b; do
  test=challenge; off=0; [ $side = b ] && test=accept && off=7
  child_pads="$(pick $off),$(mash $off)"
  ( cd "$OUT/lobby_$side" && M1_LOBBY_SERVER="${M1_LOBBY_SERVER:-none}" M1_FULLSCREEN=0 M1_VMU="$(cygpath -m "$OUT/lobby_$side")/vmu_a1.bin" M1_LOBBY_TEST=$test M1_CHILD_SECONDS=$SECS M1_CHILD_PADS="$child_pads" M1_NOSOUND=1 M1_FAST=1 M1_PADS="$PADS" \
      "$EXE" "$SCW" 60 > lobby.txt 2>&1 ) &
done
wait
# the parents exit at the match start; the matches (child processes) keep writing to the same logs
for i in $(seq 1 $((SECS + 30))); do
  done_n=0
  for side in a b; do grep -q "=== M1 stop" "$OUT/lobby_$side/lobby.txt" 2>/dev/null && [ "$(grep -c '=== M1 stop' "$OUT/lobby_$side/lobby.txt")" -ge 1 ] && grep -q "net: boot" "$OUT/lobby_$side/lobby.txt" && done_n=$((done_n + 1)); done
  [ $done_n = 2 ] && break
  sleep 1
done
for side in a b; do
  echo "== $side"; grep -E "online:|net: (boot done|running|player)|M1 stop" "$OUT/lobby_$side/lobby.txt"
done
grep -h "boot done" "$OUT"/lobby_[ab]/lobby.txt | sort | uniq -c
grep -h "checksum" "$OUT/lobby_a/lobby.txt" | sed 's/ (.*//' > "$OUT/lobby_a/sums.txt"
grep -h "checksum" "$OUT/lobby_b/lobby.txt" | sed 's/ (.*//' > "$OUT/lobby_b/sums.txt"
n=$(comm -12 <(sort "$OUT/lobby_a/sums.txt") <(sort "$OUT/lobby_b/sums.txt") | wc -l)
echo "checksums: $(wc -l < "$OUT/lobby_a/sums.txt") / $(wc -l < "$OUT/lobby_b/sums.txt") lines, $n identical"
