#!/usr/bin/env bash
# Exports the 4:3 artwork that leaves bars in 16:9 (M1_ARTDUMP, m1/artdump.c): runs the arcade
# script and each main-menu mode (M1_MENU_PICK, pressing A now and then to go deeper), four at a
# time, then merges everything into build/art/index.html (pictures that look alike once).
# The pictures are the game's: they stay on this machine. Usage: bash tests/artdump.sh [seconds]
source "$(dirname "$0")/common.sh"
T=${1:-90}
ART="$PORT/build/art"
mkdir -p "$ART"; rm -rf "${ART:?}"/*
EXPL="600:s,606:-,900:a,906:-,1200:s,1206:-"
for f in $(seq 1800 220 9000); do EXPL="$EXPL,$f:a,$((f + 6)):-"; done
# name, menu pick (menu:entry; main menu 0: 1 VS, 3 TEAM, 4 TIME ATTACK, 5 SURVIVAL, 8 PRACTICE,
# 10 OPTIONS, 11 GRAPHICS, 12 MISSION; Museum 1: 0 ART GALLERY, 1 BATTLE THEATER)
runs="arcade:- vs:0:1 team:0:3 timeattack:0:4 survival:0:5 practice:0:8 options:0:10 mission:0:12 gallery:1:0 theater:1:1"
n=0
for r in $runs; do
  name=${r%%:*}; pick=${r#*:}
  mkdir -p "$ART/$name"
  (
    cd "$ART/$name"
    if [ "$pick" = - ]; then pads="$PADS"; else pads="$EXPL"; export M1_MENU_PICK="$pick"; fi
    M1_ARTDUMP="$(cygpath -m "$ART/$name")" M1_NOSOUND=1 M1_FAST=1 M1_PADS="$pads" "$EXE" "$SCW" $T > run.txt 2>&1
    echo "$name: $(grep -c '^artdump:' run.txt) screens"
  ) &
  n=$((n + 1)); [ $((n % 4)) = 0 ] && wait
done
wait
python -I "$PORT/tests/art_merge.py" "$ART"
