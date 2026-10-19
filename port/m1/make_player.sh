#!/usr/bin/env bash
# Assembles the player folder sc/port/build/player (Windows): Voldorium.exe (GUI), m1.exe
# (console, same program), SDL2.dll, the rollback range table, the BIOS font, a starting
# voldorium.ini (template m1/player/voldorium.ini, written only when missing) and README.txt
# (m1/player/README.txt). No game data goes in: on its first start the game asks for the
# player's own disc dump (.gdi) with a file picker. tools/build.py --only player.
# Usage (Git Bash, from sc/port, after m1/build_win.sh): bash m1/make_player.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
python -I tools/build.py --only player
echo "player folder: port/build/player"
