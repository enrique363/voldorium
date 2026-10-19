#!/usr/bin/env bash
# Runs a Ghidra script headlessly against the SoulCalibur project (no re-analysis).
# usage: sc/ghidra-run.sh <Script.java> [script args...]   (Windows paths for file args)
set -euo pipefail
SC="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SC")"
export JAVA_HOME="$(cygpath -w "$ROOT/engines/jdk-21")"
script="$1"; shift
"$ROOT/engines/ghidra_12.1.4_PUBLIC/support/analyzeHeadless.bat" \
  "$(cygpath -w "$SC/ghidra")" SoulCalibur -process 1ST_READ.BIN -noanalysis \
  -scriptPath "$(cygpath -w "$SC/ghidra-scripts")" -postScript "$script" "$@" 2>&1 |
  grep -E "ERROR|Exception|> |REPORT: (Post|Save)" || true
