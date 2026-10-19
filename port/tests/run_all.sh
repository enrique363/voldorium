#!/usr/bin/env bash
# Builds the Windows executables and runs every test (Git Bash, from anywhere).
# About 13 minutes: oracle regressions (WSL), SyncTest, two-peer netplay, screenshots, lobby, dropped match.
source "$(dirname "$0")/common.sh"
cd "$PORT"
echo "== build"; bash m1/build_win.sh 2>&1 | grep -E "error:|^built"
echo "== oracle (WSL)"; MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash "/mnt/$(echo "$PORT" | sed 's|^/\(.\)/|\1/|')/tests/oracle.sh"
echo "== synctest"; bash tests/synctest.sh 120
echo "== netplay"; bash tests/netplay.sh 170
echo "== screens"; bash tests/screens.sh
echo "== lobby"; bash tests/lobby.sh 90 | tail -3
echo "== internet lobby (local server)"; bash tests/lobby_inet.sh relay 60 | tail -4
echo "== lobby match dropped"; bash tests/lobby_drop.sh 45 | tail -1
