#!/usr/bin/env bash
# Broad oracle (M0): records up to SC_PROBE_MAX calls of every function entry during an autoplay
# fight, with each call's full memory/hardware access log, in the oracle Flycast (~/sc-flycast-m0).
# usage (WSL): bash capture_oracle_broad.sh [seconds] [max calls per function]
# Output: /tmp/oracle_broad.jsonl (large; pack it with oracle_pack.py, replay with replay.c)
cd ~
SC="$(cd "$(dirname "$0")/../.." && pwd)"
C="$SC/disc/Soulcalibur (USA).chd"
export GALLIUM_DRIVER=d3d12 MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA SDL_VIDEODRIVER=x11
export SC_TRACE=1 SC_TRACE_FILE=/tmp/m0_x.csv SC_TRACE_FROM=999999
export SC_SHOTS=99999 SC_AUTOPLAY=1 SC_SYNC_LOG=/tmp/m0_sync.csv SC_HEARTBEAT=1
export SC_PROBE_FILE="$SC/port/m0/broad_entries.txt"   # every entry except sh4_ReadTimer (faked)
export SC_PROBE_ACCESS=1 SC_PROBE_ARM=1
export SC_PROBE_MAX="${2:-10}"
export SC_PROBE_OUT=/tmp/oracle_broad.jsonl
LOG=/tmp/m0_oracle_broad.log
# WSLg sometimes starts the emulator without it ever running (no heartbeat): retry those starts
for attempt in 1 2 3 4; do
	./sc-flycast-m0/build/flycast "$C" > $LOG 2>&1 &
	pid=$!
	sleep 45
	if grep -q "SC heartbeat" $LOG; then
		echo "attempt $attempt: running"
		break
	fi
	echo "attempt $attempt: no heartbeat, restarting"
	kill $pid 2>/dev/null; wait $pid 2>/dev/null
	pid=
done
[ -n "$pid" ] || { echo "emulator never started"; exit 1; }
sleep "$(( ${1:-420} - 45 ))"
kill $pid 2>/dev/null; wait $pid 2>/dev/null
grep "SC probe" $LOG
grep "SC heartbeat" $LOG | tail -1
python3 -c "
import json,collections
ok=collections.Counter(); skip=collections.Counter()
for l in open('/tmp/oracle_broad.jsonl'):
    try: d=json.loads(l)
    except Exception: continue
    if 'skip' in d: skip[d['skip']]+=1
    else: ok[d['fn']]+=1
print(len(ok),'functions with replayable calls,',sum(ok.values()),'calls; skipped:',dict(skip))"
ls -la /tmp/oracle_broad.jsonl
