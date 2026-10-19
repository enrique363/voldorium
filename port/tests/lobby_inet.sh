#!/usr/bin/env bash
# Internet lobby on one PC: a local lobby server (lobby/sc_lobby.py on UDP 7200/7201), two game
# copies that find each other only through it (M1_LOBBY_LAN=0), challenge, accept, punch and play
# (tests/lobby.sh). Usage: bash tests/lobby_inet.sh [direct|relay] [match seconds]
source "$(dirname "$0")/common.sh"
MODE=${1:-direct}
SC_LOBBY_PORT=7200 python -I "$PORT/lobby/sc_lobby.py" > "$OUT/lobby_server.txt" 2>&1 &
SRV=$!
sleep 1
export SC_LOBBY_PORT=7200 M1_LOBBY_SERVER=127.0.0.1:7200 M1_LOBBY_LAN=0
[ "$MODE" = relay ] && export M1_LOBBY_FORCE_RELAY=1
bash "$PORT/tests/lobby.sh" "${2:-60}"
kill $SRV 2>/dev/null
echo "== server log"; grep -E "join|match|relay" "$OUT/lobby_server.txt" | head -12
