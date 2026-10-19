#!/usr/bin/env bash
# Builds the traced Flycast in WSL (~/sc-flycast). Run inside Ubuntu:
#   bash /mnt/c/Users/kiker/ReverseEngineering/sc/flycast-trace/build.sh
set -euo pipefail
SRC="$HOME/sc-flycast"
HERE="$(cd "$(dirname "$0")" && pwd)"
git -C "$SRC" checkout -- core/hw/sh4/sh4_mem.cpp core/hw/sh4/storeq.cpp core/emulator.cpp core/hw/sh4/interpr/sh4_interpreter.cpp
python3 "$HERE/apply_trace_hook.py" "$SRC"
python3 "$HERE/apply_memtrace_hook.py" "$SRC"
python3 "$HERE/apply_synctest_hook.py" "$SRC"
cmake -S "$SRC" -B "$SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$SRC/build" flycast
ls -la "$SRC/build/flycast"
