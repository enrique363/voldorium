#!/usr/bin/env bash
# Runs Soulcalibur in the traced Flycast. Run inside Ubuntu:
#   bash /mnt/c/path/to/sc/flycast-trace/run_trace.sh
# Then start any fight and let round 1 run ~3 seconds after "FIGHT!!". Close Flycast afterwards.
set -euo pipefail
# Render on the NVIDIA GPU through WSL D3D12 (default is slow software OpenGL)
export GALLIUM_DRIVER=d3d12
export MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA
SC="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$SC/traces"
export SC_TRACE=1
export SC_TRACE_FILE="$SC/traces/hud_trace.csv"
export SC_TRACE_FROM="${SC_TRACE_FROM:-120}"
export SC_TRACE_FRAMES="${SC_TRACE_FRAMES:-3}"
exec "$HOME/sc-flycast/build/flycast" "$SC/disc/Soulcalibur (USA).chd"
