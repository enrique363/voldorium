#!/usr/bin/env bash
# Unattended SyncTest over several character / stage / mode combinations (SC_AUTOPLAY_SEED).
#   wsl -d Ubuntu -- bash /mnt/c/Users/kiker/ReverseEngineering/sc/flycast-trace/run_synctest_combos.sh 1500 1 2 5 8
# args: frames, then one or more seeds. Output per seed: sc/traces/combo/s<seed>/{run.log,synctest.csv,
# statelog.csv,sync_<n>_*.{bin,txt}}. Needs "[input]\ndevice2 = 0" in ~/.config/flycast/emu.cfg for VS Battle.
# Audio is switched off (-config audio:backend=null): with the WSLg PulseAudio sink the emulator thread
# can deadlock at boot, and without audio pacing the test runs faster than real time.
# Runs from files and never pkill -x flycast (other sessions may run their own builds).
set -u
SC=/mnt/c/Users/kiker/ReverseEngineering/sc
FRAMES=$1; shift
cd ~
for SEED in "$@"; do
	OUT=$SC/traces/combo/s$SEED
	mkdir -p "$OUT"; rm -f "$OUT"/*
	export GALLIUM_DRIVER=${GALLIUM_DRIVER:-d3d12} MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA SDL_VIDEODRIVER=x11
	export SC_HEARTBEAT=1 SC_TRACE=1 SC_TRACE_FILE=/tmp/x_s$SEED.csv SC_TRACE_FROM=999999
	export SC_SYNCTEST=1 SC_AUTOPLAY=1 SC_AUTOPLAY_LOG=1 SC_SYNC_ANYSTATE=1 SC_AUTOPLAY_SEED=$SEED
	export SC_SYNC_RANGES="${RANGES:-$SC/ggpo/savestate_ranges.tsv}"
	export SC_SYNC_FRAMES=$FRAMES SC_SYNC_LOG="$OUT/synctest.csv" SC_SYNC_DUMP=8 SC_SYNC_DUMPDIR="$OUT"
	export SC_STATELOG="$OUT/statelog.csv"
	export SC_MEMTRACE=1 SC_MEM_FROM=999999 SC_MEM_TO=999999 SC_MEM_DIR=/tmp
	./sc-flycast/build/flycast -config config:rend.vsync=no -config audio:backend=null "$SC/disc/Soulcalibur (USA).chd" > "$OUT/run.log" 2>&1 &
	pid=$!
	for _ in $(seq 1 ${WAIT_STEPS:-400}); do
		sleep 5
		kill -0 $pid 2>/dev/null || break
		[ "$(grep -c '' "$OUT/synctest.csv" 2>/dev/null)" -gt "$FRAMES" ] && break
	done
	kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
	echo "seed $SEED: $(grep 'synctest:' "$OUT/run.log" | tail -1)"
done
