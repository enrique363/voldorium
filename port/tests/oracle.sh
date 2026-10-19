#!/usr/bin/env bash
# Translator regressions (WSL): broad oracle replay (expect 5,577 exact; the 162 failures are
# the HLE boundaries 8c001006 / 8c102e10) and the M0 integer / FPU suites (683/683, 2800/2800).
# Run from Windows: MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash /mnt/c/.../sc/port/tests/oracle.sh

cd "$(dirname "$0")/.."
bash m1/build.sh 2>&1 | grep -E "error:|^built"
bash m0/build_replay.sh 2>&1 | tail -1
python3 -I m0/oracle_pack.py build/oracle_broad.jsonl $HOME/m1_build/oracle_broad.bin >/dev/null
/tmp/replay $HOME/m1_build/oracle_broad.bin build/replay_broad.tsv
cut -f3 build/replay_broad.tsv | grep -v "^$" | sed 's/ (.*//' | sort | uniq -c
python3 -I sh4c/sh4c.py /tmp/m0_funcs.c 8c0639c8 8c0fed6c 8c0fe6fc >/dev/null && gcc -O2 -w -I sh4c -o /tmp/m0_int /tmp/m0_funcs.c m0/tests.c && /tmp/m0_int | tail -1
E="8c063a00 8c03dfa0 8c03c73e 8c03c6ae 8c03c6d6 8c03c7c0 8c03c886 8c03dc38 8c0c08f2 8c0c0942 8c0c09ae 8c03e9c0 8c060f54 8c060f9c 8c060da0"
python3 -I sh4c/sh4c.py /tmp/m0_fpu.c $E >/dev/null && gcc -O2 -ffp-contract=off -w -I sh4c -I ~/sc-flycast-m0/core/hw/sh4 -o /tmp/m0_fpu /tmp/m0_fpu.c sh4c/fsca.c m0/tests_fpu.c -lm && /tmp/m0_fpu ../extracted/1ST_READ.BIN | tail -1
