#!/usr/bin/env bash
# Builds the native Windows executables (m1.exe, Voldorium.exe) with llvm-mingw and SDL2.
# Run from Git Bash in port/: bash m1/build_win.sh (after sh4c/sweep.py, see the README).
# Tools, by default in ../engines next to this repository (each can be overridden):
#   LLVM_MINGW  llvm-mingw (x86_64 ucrt)          SDL2_DIR  SDL2 development files (mingw)
#   FLYCAST     a Flycast source checkout (GPL-2): its FSCA sine table header, and its BIOS font
# Output goes outside the project, to %LOCALAPPDATA%/sc-m1.
# Then: cd build/m1 && "$LOCALAPPDATA/sc-m1/m1.exe" ../.. [seconds]
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f ../local.env ] && . ../local.env    # this PC's tool locations (not in git)
ENG=../../engines
LLVM_MINGW=${LLVM_MINGW:-$ENG/llvm-mingw-20260922-ucrt-x86_64}
CC=$LLVM_MINGW/bin/x86_64-w64-mingw32-clang
SDL=${SDL2_DIR:-$ENG/SDL2-2.32.10/x86_64-w64-mingw32}
FLY=${FLYCAST:-$ENG/flycast}
OUT=$(cygpath -u "$LOCALAPPDATA")/sc-m1
O=$OUT/obj; mkdir -p $O build/m1
[ -f $OUT/biosfont.bin ] || [ ! -f "$FLY/fonts/biosfont.bin.zip" ] || python -I -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extract('biosfont.bin', sys.argv[2])" "$FLY/fonts/biosfont.bin.zip" "$OUT"
cp -u $SDL/bin/SDL2.dll $OUT/
CF="-O1 -ffp-contract=off -fno-strict-aliasing -I sh4c -DSH4_TAG_WRITES -w"
# translated code: rebuild only when the generated file is newer than its object
for f in build/sweep/all_*.c build/sweep/dispatch.c; do
  o=$O/$(basename $f .c).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ sh4c/sh4rt.h -nt $o ]; then $CC $CF -c -o $o $f & fi
done
[ -f $O/fsca.o ] || $CC $CF -I $FLY/core/hw/sh4 -c -o $O/fsca.o sh4c/fsca.c &
# lobby build key: players only see and match copies built from the same sources and state table
SCB=$(cat m1/*.c m1/*.h m1/*.S sh4c/sh4rt.h ../ggpo/savestate_ranges.tsv | sha1sum | cut -c1-12)
for f in m1/m1.c m1/hw.c m1/hle.c m1/threads.c m1/maple.c m1/ta.c m1/tex.c m1/render.c m1/state.c m1/netplay.c m1/aica.c m1/arm7.c m1/config.c m1/online.c m1/artdump.c m1/artswap.c m1/png.c; do
  $CC ${CF/-w/} -Wall -I $SDL/include -I $ENG/ggpo/src/include -I m1 -DSC_BUILD_ID=\"$SCB\" -c -o $O/m1_$(basename $f .c).o $f &
done
$CC -c -o $O/sjlj_win64.o m1/sjlj_win64.S &
# the icon and version info (m1/voldorium.rc, icon drawn by art/make_icon.py)
$LLVM_MINGW/bin/x86_64-w64-mingw32-windres -I m1 -O coff -o $O/voldorium_res.o m1/voldorium.rc &
wait
# 64 MB main stack (Linux gives 8 MB; the SDK threads get 8 MB fibers)
# GGPO (engines/ggpo, C++): its static library is built by m1/build_ggpo.sh; link with clang++
LIBS="$OUT/ggpo/libggpo.a -L $SDL/lib -lSDL2 -lopengl32 -lwinmm -lws2_32 -lcomdlg32 -lgdi32 -static-libstdc++ -static-libgcc -Wl,--stack,67108864"
${CC}++ -o $OUT/m1.exe $O/*.o $LIBS
# the player's executable: same code, GUI subsystem (no console window; the log goes to m1_log.txt)
${CC}++ -mwindows -o $OUT/Voldorium.exe $O/*.o $LIBS
echo built $OUT/m1.exe $OUT/Voldorium.exe
