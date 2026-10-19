#!/usr/bin/env bash
# Builds the native Windows executables (m1.exe, Voldorium.exe) into %LOCALAPPDATA%/sc-m1:
# tools/build.py --only game (the build steps live there). Run from Git Bash in port/, after
# sh4c/sweep.py and m1/build_ggpo.sh. Tools: see tools/build.py (LLVM_MINGW, SDL2_DIR, FLYCAST in
# local.env, ../engines next to the repository, or toolchain/ from tools/build.py --download).
# Then: cd build/m1 && "$LOCALAPPDATA/sc-m1/m1.exe" ../.. [seconds]
set -euo pipefail
cd "$(dirname "$0")/.."
python -I ../tools/build.py --only game | sed 's/^  built Voldorium.exe.*/built m1.exe Voldorium.exe/;s/^\[4\/5\] .*//' | grep -v '^$'
