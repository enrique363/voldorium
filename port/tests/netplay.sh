#!/usr/bin/env bash
# Two netplay peers on this machine (UDP 7000/7001) with 100 ms simulated latency, both pressing
# buttons through a fight; compares their state checksums. Usage: bash tests/netplay.sh [seconds]
# NET_WINDOWED=1: in windows with VSync on (frame times logged), instead of headless
source "$(dirname "$0")/common.sh"
T=${1:-170}
mkdir -p "$OUT/net1" "$OUT/net2"
mash() { local s="" off=$1; for i in $(seq 0 110); do f=$((4400+off+i*20)); case $(((i+off)%5)) in 0) b=a;; 1) b=b;; 2) b=x;; 3) b=u;; 4) b=d;; esac; s="$s,$f:$b,$((f+4)):-"; done; echo "${s#,}"; }
P1="$PADS,$(mash 0)"; P2="$(mash 7)"
VIDEO="M1_HEADLESS=1"
[ "${NET_WINDOWED:-0}" = 1 ] && VIDEO="M1_FULLSCREEN=0 M1_VSYNC=${NET_VSYNC:-1} M1_FRAMETIME=1 M1_NOSOUND=1"
(cd "$OUT/net1" && env "ggpo.network.delay=100" $VIDEO M1_NET=7000,1,127.0.0.1:7001 M1_PADS="$P1" "$EXE" "$SCW" $T > out.txt 2>&1 &)
(cd "$OUT/net2" && env "ggpo.network.delay=100" $VIDEO M1_NET=7001,2,127.0.0.1:7000 M1_PADS="$P2" "$EXE" "$SCW" $T > out.txt 2>&1 &)
sleep $((T + 8))
for d in net1 net2; do
  grep "checksum" "$OUT/$d/out.txt" | awk '{print $3, $5}' > "$OUT/$d/sums.txt"
  echo "$d: $(grep '^net: [0-9]* frames' "$OUT/$d/out.txt")"
done
if diff -q "$OUT/net1/sums.txt" "$OUT/net2/sums.txt" > /dev/null; then
  echo "checksums identical on both peers ($(wc -l < "$OUT/net1/sums.txt") reports)"
else
  echo "DESYNC:"; diff "$OUT/net1/sums.txt" "$OUT/net2/sums.txt" | head
fi
