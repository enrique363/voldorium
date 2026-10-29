#!/usr/bin/env bash
# Builds the Windows executables and runs every test (Git Bash, from anywhere).
# About 22 minutes: oracle regressions (WSL), SyncTest, two-peer netplay, screenshots, lobby, dropped match,
# every main menu mode, and Windows against Linux (netplay and a lobby match).
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
echo "== every main menu mode"; bash tests/modes.sh 60 | tail -1
echo "== Windows against Linux (WSL build)"; MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash "/mnt/$(echo "$PORT" | sed 's|^/\(.\)/|\1/|')/m1/build.sh" | tail -1
bash tests/crossplay.sh 120 | tail -1
bash tests/lobby_cross.sh relay 60 | grep "checksums"
