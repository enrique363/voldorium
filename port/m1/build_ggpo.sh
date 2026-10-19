#!/usr/bin/env bash
# Builds GGPO (third_party/ggpo: pond3r/ggpo at 7ddadef, MIT, see its LICENSE) as a static library
# for the Windows executable: %LOCALAPPDATA%/sc-m1/ggpo/libggpo.a. Two local changes, both marked
# "sc port": an MSVC-only extra qualification removed in network/udp_proto.h, and
# ggpo_set_prediction_window (lockstep outside fights, see m1/netplay.c).
# LLVM_MINGW: the llvm-mingw toolchain folder (default ../engines/llvm-mingw-20260922-ucrt-x86_64
# next to this repository).
set -euo pipefail
cd "$(dirname "$0")/../.."
[ -f local.env ] && . ./local.env
LLVM_MINGW=${LLVM_MINGW:-$(cd .. && pwd)/engines/llvm-mingw-20260922-ucrt-x86_64}
cd third_party/ggpo
B=$LLVM_MINGW/bin
OUT=$(cygpath -u "$LOCALAPPDATA")/sc-m1/ggpo; mkdir -p "$OUT"
for f in src/lib/ggpo/*.cpp src/lib/ggpo/backends/*.cpp src/lib/ggpo/network/*.cpp; do
  case $f in *platform_linux*) continue;; esac
  $B/x86_64-w64-mingw32-clang++ -O2 -w -std=c++14 -D_WINDOWS -I src/include -I src/lib/ggpo -c -o "$OUT/$(basename $f .cpp).o" $f &
done
wait
$B/llvm-ar rcs "$OUT/libggpo.a" "$OUT"/*.o
echo built "$OUT/libggpo.a"
