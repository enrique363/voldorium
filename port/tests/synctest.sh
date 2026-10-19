#!/usr/bin/env bash
# Native SyncTest in fights: every fight frame is saved, run, rewound and run again; prints
# "SyncTest: n/n frames identical". Usage: bash tests/synctest.sh [seconds]
source "$(dirname "$0")/common.sh"
cd "$OUT"
M1_SYNCTEST=1 M1_FAST=1 M1_NOSOUND=1 M1_PADS="$PADS" "$EXE" "$SCW" "${1:-120}" > synctest.txt 2>&1
grep "SyncTest:" synctest.txt | tail -1
grep -c "SyncTest mismatch" synctest.txt | sed 's/^/mismatches: /'
