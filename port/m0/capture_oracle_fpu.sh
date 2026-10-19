#!/usr/bin/env bash
# Records real calls of the FPU validation set in the oracle Flycast (~/sc-flycast-m0).
# usage (WSL): bash capture_oracle_fpu.sh [seconds]
cd ~
SC="$(cd "$(dirname "$0")/../.." && pwd)"
C="$SC/disc/Soulcalibur (USA).chd"
export GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA SDL_VIDEODRIVER=x11
export SC_TRACE=1 SC_TRACE_FILE=/tmp/m0_x.csv SC_TRACE_FROM=999999
export SC_SHOTS=99999 SC_AUTOPLAY=1 SC_SYNC_LOG=/tmp/m0_sync.csv
FNS="0x8c063a00 0x8c03dfa0 0x8c03c73e 0x8c03c6ae 0x8c03c6d6 0x8c03c7c0 0x8c03c886 0x8c03dc38 0x8c060f54 0x8c060f9c 0x8c060da0"
MEM="0x8c063a00:0x8c159060:4"
for f in $FNS; do
	[ "$f" = 0x8c063a00 ] && continue
	MEM="$MEM,$f:r4:64,$f:r5:64,$f:r6:64"
done
MEM="$MEM,0x8c0c08f2:r4:16,0x8c0c08f2:r5:16"
MEM="$MEM,0x8c0c0942:r4:r6*16,0x8c0c0942:r5:r6*16,0x8c0c09ae:r4:r6*16,0x8c0c09ae:r5:r6*16"
MEM="$MEM,0x8c03e9c0:r4:64,0x8c03e9c0:0x8c266c00:512,0x8c03e9c0:0x8c25e1d0:96,0x8c03e9c0:0x8c2627a0:128"
export SC_PROBE="$(echo $FNS | tr ' ' ','),0x8c0c08f2,0x8c0c0942,0x8c0c09ae,0x8c03e9c0"
export SC_PROBE_MEM="$MEM"
export SC_PROBE_MAX=200
export SC_PROBE_OUT="$SC/port/m0/oracle_fpu.jsonl"
./sc-flycast-m0/build/flycast "$C" > /tmp/m0_oracle_fpu.log 2>&1 &
pid=$!
sleep "${1:-300}"
kill $pid 2>/dev/null; wait $pid 2>/dev/null
python3 -c "
import json,collections
c=collections.Counter(json.loads(l)['fn'] for l in open('$SC/port/m0/oracle_fpu.jsonl'))
print(dict(c))"
