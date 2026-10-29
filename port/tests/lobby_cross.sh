#!/usr/bin/env bash
# Online Versus lobby, Windows against Linux: a local lobby server (lobby/sc_lobby.py), the Windows
# build challenges and the Linux build in WSL accepts (or the other way round); they find each
# other only through the server (M1_LOBBY_LAN=0), punch, restart into the match and play, as
# tests/lobby.sh. Passes when both boots end at the same loop and the netplay checksums match.
# Usage (Git Bash): bash tests/lobby_cross.sh [direct|relay] [match seconds] [windows: challenge|accept]
source "$(dirname "$0")/common.sh"
MODE=${1:-direct}; SECS=${2:-60}; WT=${3:-challenge}
[ "$WT" = challenge ] && LT=accept || LT=challenge
WIN_IP=$(wsl.exe -e sh -c "ip route | awk '/default/ {print \$3}'" | tr -d '\r')
SRV_AT="$WIN_IP:7200"   # the address both reach; the server sees each side at its own address
SC_LOBBY_PORT=7200 python -I "$PORT/lobby/sc_lobby.py" > "$OUT/lobby_server.txt" 2>&1 &
SRV=$!
sleep 1
RELAY=""; [ "$MODE" = relay ] && RELAY="M1_LOBBY_FORCE_RELAY=1"
mash() { local s="" off=$1; for i in $(seq 0 150); do f=$((1500+off+i*20)); case $(((i+off)%5)) in 0) b=a;; 1) b=b;; 2) b=x;; 3) b=u;; 4) b=d;; esac; s="$s,$f:$b,$((f+4)):-"; done; echo "${s#,}"; }
pick() { local s="" off=$1; for i in $(seq 0 12); do f=$((150+off+i*90)); s="$s,$f:a,$((f+6)):-"; done; echo "${s#,}"; }
mkdir -p "$OUT/lobby_win" "$OUT/lobby_lin"
rm -f "$OUT/lobby_win/lobby.txt" "$OUT/lobby_lin/lobby.txt"
( cd "$OUT/lobby_win" && env $RELAY M1_LOBBY_SERVER=$SRV_AT M1_LOBBY_LAN=0 M1_FULLSCREEN=0 M1_VMU="$(cygpath -m "$OUT/lobby_win")/vmu_a1.bin" \
    M1_LOBBY_TEST=$WT M1_CHILD_SECONDS=$SECS M1_CHILD_PADS="$(pick 0),$(mash 0)" M1_NOSOUND=1 M1_FAST=1 M1_PADS="$PADS" \
    "$EXE" "$SCW" 60 > lobby.txt 2>&1 ) &
WPID=$!
LDIR=/mnt/c${OUT#/c}/lobby_lin
LEXE=${LINUX_EXE:-'$HOME/m1_build/m1'}   # LINUX_EXE: another Linux build (a path in WSL)
MSYS_NO_PATHCONV=1 wsl.exe -e bash -c "cd '$LDIR' && env $RELAY M1_LOBBY_SERVER=$SRV_AT M1_LOBBY_LAN=0 M1_FULLSCREEN=0 M1_VMU='$LDIR/vmu_a1.bin' \
    M1_LOBBY_TEST=$LT M1_CHILD_SECONDS=$SECS M1_CHILD_PADS='$(pick 7),$(mash 7)' M1_NOSOUND=1 M1_FAST=1 M1_PADS='$PADS' \
    $LEXE /mnt/c${SC#/c} 60 > lobby.txt 2>&1; while pgrep -f -- '[-]-disc' > /dev/null; do sleep 1; done" &
LPID=$!
wait $WPID $LPID   # not the server
for i in $(seq 1 $((SECS + 30))); do
  n=0
  for d in win lin; do grep -q "=== M1 stop" "$OUT/lobby_$d/lobby.txt" 2>/dev/null && grep -q "net: boot" "$OUT/lobby_$d/lobby.txt" && n=$((n + 1)); done
  [ $n = 2 ] && break
  sleep 1
done
kill $SRV 2>/dev/null
for d in win lin; do
  echo "== $d"; grep -E "online:|net: (boot done|running|player|registered)|M1 stop" "$OUT/lobby_$d/lobby.txt"
done
grep -h "boot done" "$OUT"/lobby_win/lobby.txt "$OUT"/lobby_lin/lobby.txt | sort | uniq -c
for d in win lin; do grep -h "checksum" "$OUT/lobby_$d/lobby.txt" | sed 's/ (.*//' > "$OUT/lobby_$d/sums.txt"; done
n=$(comm -12 <(sort "$OUT/lobby_win/sums.txt") <(sort "$OUT/lobby_lin/sums.txt") | wc -l)
echo "checksums: $(wc -l < "$OUT/lobby_win/sums.txt") / $(wc -l < "$OUT/lobby_lin/sums.txt") lines, $n identical"
echo "== server log"; grep -E "join|match|relay" "$OUT/lobby_server.txt" | head -8
