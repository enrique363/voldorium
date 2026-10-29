#!/usr/bin/env bash
# Windows against Linux: the Windows build (m1.exe) and the Linux build in WSL (bash m1/build.sh)
# play one netplay session over WSL's virtual network with 100 ms simulated latency, both pressing
# buttons through a fight, as tests/netplay.sh; compares their state checksums.
# Usage (Git Bash): bash tests/crossplay.sh [seconds] [windows player: 1 or 2]
source "$(dirname "$0")/common.sh"
T=${1:-170}; WP=${2:-1}; LP=$((3 - WP))
WSL_IP=$(wsl.exe -e hostname -I | awk '{print $1}')
WIN_IP=$(wsl.exe -e sh -c "ip route | awk '/default/ {print \$3}'" | tr -d '\r')
mkdir -p "$OUT/xwin" "$OUT/xlin"
mash() { local s="" off=$1; for i in $(seq 0 110); do f=$((4400+off+i*20)); case $(((i+off)%5)) in 0) b=a;; 1) b=b;; 2) b=x;; 3) b=u;; 4) b=d;; esac; s="$s,$f:$b,$((f+4)):-"; done; echo "${s#,}"; }
P1="$PADS,$(mash 0)"; P2="$(mash 7)"
[ "$WP" = 1 ] && { PW="$P1"; PL="$P2"; } || { PW="$P2"; PL="$P1"; }
echo "Windows player $WP at $WIN_IP:7000, Linux player $LP at $WSL_IP:7001"
LDIR=/mnt/c${OUT#/c}/xlin
LEXE=${LINUX_EXE:-'$HOME/m1_build/m1'}   # LINUX_EXE: another Linux build (a path in WSL)
MSYS_NO_PATHCONV=1 wsl.exe -e bash -c "cd '$LDIR' && env ggpo.network.delay=100 M1_HEADLESS=1 M1_NET=7001,$LP,$WIN_IP:7000 M1_PADS='$PL' $LEXE /mnt/c${SC#/c} $T > out.txt 2>&1" &
(cd "$OUT/xwin" && env "ggpo.network.delay=100" M1_HEADLESS=1 M1_NET=7000,$WP,$WSL_IP:7001 M1_PADS="$PW" "$EXE" "$SCW" $T > out.txt 2>&1 &)
sleep $((T + 8)); wait
for d in xwin xlin; do
  grep "checksum" "$OUT/$d/out.txt" | awk '{print $3, $5}' > "$OUT/$d/sums.txt"
  echo "$d: $(grep '^net: [0-9]* frames' "$OUT/$d/out.txt")"
done
# the frames both reported (the side that reaches its time limit first stops a report early)
n=$(join <(sort "$OUT/xwin/sums.txt") <(sort "$OUT/xlin/sums.txt") | wc -l)
bad=$(join <(sort "$OUT/xwin/sums.txt") <(sort "$OUT/xlin/sums.txt") | awk '$2 != $3' | head)
if [ "$n" -gt 0 ] && [ -z "$bad" ]; then
  echo "checksums identical on Windows and Linux ($n reports)"
else
  echo "DESYNC or no session ($n common reports):"; echo "$bad"
fi
