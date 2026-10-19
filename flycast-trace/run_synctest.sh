#!/usr/bin/env bash
# GGPO-style SyncTest. From Windows:
#   wsl -d Ubuntu -- bash /mnt/c/path/to/sc/flycast-trace/run_synctest.sh
# Control run with the real SH4 timer (expected to mismatch):
#   wsl -d Ubuntu -- bash /mnt/c/path/to/sc/flycast-trace/run_synctest.sh realtimer
# Start a fight and play actively. Each fight frame is simulated twice, so the game runs at
# about half speed. 600 frames are tested (~20 s), then testing stops; close Flycast.
# Output: sc/traces/synctest.csv (or synctest_realtimer.csv)
set -euo pipefail
export GALLIUM_DRIVER=d3d12
export MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA
SC="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$SC/traces"
export SC_TRACE=1                       # forces the interpreter
export SC_TRACE_FILE=/tmp/sc_hud_unused.csv
export SC_TRACE_FROM=999999             # HUD trace effectively off
export SC_SYNCTEST=1
export SC_SYNC_RANGES="$SC/ggpo/savestate_ranges.tsv"
export SC_SYNC_FRAMES="${SC_SYNC_FRAMES:-600}"
export SC_SYNC_DUMP="${SC_SYNC_DUMP:-4}"          # full snapshots of the first mismatching frames
export SC_SYNC_DUMPDIR="$SC/traces"
if [ "${1:-}" = realtimer ]; then
	export SC_SYNC_TIMER=0
	export SC_SYNC_LOG="$SC/traces/synctest_realtimer.csv"
else
	export SC_SYNC_LOG="$SC/traces/synctest.csv"
fi
exec "$HOME/sc-flycast/build/flycast" "$SC/disc/Soulcalibur (USA).chd"
