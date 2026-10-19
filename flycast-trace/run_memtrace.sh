#!/usr/bin/env bash
# Memory-footprint capture for GGPO save-state design. Run inside Ubuntu, or from Windows:
#   wsl -d Ubuntu -- bash /mnt/c/Users/kiker/ReverseEngineering/sc/flycast-trace/run_memtrace.sh
# Start a VS or Arcade fight and FIGHT actively (move, attack, block, get hit, jump) for
# ~12 seconds after "FIGHT!!" so the round reaches battle-state frame 720. Then close Flycast.
# Output: sc/traces/mem_writes.csv, mem_hwreads.csv, mem_reads.csv
set -euo pipefail
# Render on the NVIDIA GPU through WSL D3D12 (default is slow software OpenGL)
export GALLIUM_DRIVER=d3d12
export MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA
SC=/mnt/c/Users/kiker/ReverseEngineering/sc
mkdir -p "$SC/traces"
export SC_TRACE=1                       # forces the interpreter (accurate PC/PR)
export SC_TRACE_FILE="$SC/traces/hud_trace_2.csv"
export SC_MEMTRACE=1
export SC_MEM_DIR="$SC/traces"
export SC_MEM_FROM="${SC_MEM_FROM:-120}"
export SC_MEM_TO="${SC_MEM_TO:-720}"
exec "$HOME/sc-flycast/build/flycast" "$SC/disc/Soulcalibur (USA).chd"
