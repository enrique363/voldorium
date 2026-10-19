#!/usr/bin/env bash
# Records real calls of the M0 target functions in the oracle Flycast (~/sc-flycast-m0).
# usage (WSL): bash capture_oracle.sh [seconds]
cd ~
SC="$(cd "$(dirname "$0")/../.." && pwd)"
C="$SC/disc/Soulcalibur (USA).chd"
export GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA SDL_VIDEODRIVER=x11
export SC_TRACE=1 SC_TRACE_FILE=/tmp/m0_x.csv SC_TRACE_FROM=999999     # interpreter
export SC_SHOTS=99999 SC_AUTOPLAY=1 SC_SYNC_LOG=/tmp/m0_sync.csv         # hook on: fake timer + autoplay
export SC_PROBE=0x8c0639c8,0x8c0fed6c,0x8c0fe6fc
export SC_PROBE_MEM=0x8c0fed6c:r4:r6,0x8c0639c8:0x8c159060:4
export SC_PROBE_MAX=300
export SC_PROBE_OUT="$SC/port/m0/oracle_calls.jsonl"
./sc-flycast-m0/build/flycast "$C" > /tmp/m0_oracle.log 2>&1 &
pid=$!
sleep "${1:-240}"
kill $pid 2>/dev/null; wait $pid 2>/dev/null
python3 -c "
import json,collections
c=collections.Counter(json.loads(l)['fn'] for l in open('$SC/port/m0/oracle_calls.jsonl'))
print(dict(c))"
