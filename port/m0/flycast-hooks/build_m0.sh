#!/usr/bin/env bash
# Builds the M0 oracle Flycast (~/sc-flycast-m0) from the hook snapshots in this folder.
set -euo pipefail
SRC="$HOME/sc-flycast-m0"
HERE="$(cd "$(dirname "$0")" && pwd)"
git -C "$SRC" checkout -- core/hw/sh4/sh4_mem.cpp core/hw/sh4/storeq.cpp core/emulator.cpp core/hw/sh4/interpr/sh4_interpreter.cpp core/hw/aica/sgc_if.cpp
python3 "$HERE/apply_trace_hook.py" "$SRC"
python3 "$HERE/apply_memtrace_hook.py" "$SRC"
python3 "$HERE/apply_synctest_hook.py" "$SRC"
python3 "$HERE/apply_probe_hook.py" "$SRC"
python3 "$HERE/apply_wav_hook.py" "$SRC"
cmake -S "$SRC" -B "$SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
ninja -C "$SRC/build" flycast
ls -la "$SRC/build/flycast"
