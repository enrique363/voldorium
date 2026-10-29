#!/usr/bin/env bash
# Builds the Linux executable (WSL or any Linux with gcc, g++, SDL2 and OpenGL headers): the
# runtime, GGPO, and the game module sc_game.so (build/sweep). The lobby's build key is the one
# tools/build.py gives the Windows build, so both meet in the same lobby.
# usage (from sc/port): bash m1/build.sh            -> ~/m1_build/m1 (also linked as /tmp/m1)
#        then: cd build/m1 && /tmp/m1 ../.. [seconds]   (m1_hw.txt is written to the cwd)
set -euo pipefail
cd "$(dirname "$0")/.."
O=$HOME/m1_build/obj; mkdir -p $O build/m1
# the system font M1 serves at the BIOS font address: Flycast's replacement font, from the
# local Flycast checkout (not copied into this project)
# console flash settings: a copy of the flash image Flycast keeps for this console
[ -f $HOME/m1_build/dc_flash.bin ] || cp $HOME/.local/share/flycast/dc_nvmem.bin $HOME/m1_build/dc_flash.bin
[ -f $HOME/m1_build/biosfont.bin ] || python3 -c "import zipfile; zipfile.ZipFile('$HOME/sc-flycast-m0/fonts/biosfont.bin.zip').extract('biosfont.bin', '$HOME/m1_build')"
CF="-O1 -ffp-contract=off -fno-strict-aliasing -I sh4c -DSH4_TAG_WRITES"
# the build key and translator name, as tools/build.py computes them for Windows
read BID TID < <(cd .. && python3 -I -c "import sys; sys.path.insert(0, 'tools'); import build; print(build.build_id(), build.translator_id())")
# GGPO (third_party/ggpo), built once
GG=$HOME/m1_build/ggpo; GS=../third_party/ggpo/src
if [ ! -f $GG/libggpo.a ] || [ -n "$(find $GS -newer $GG/libggpo.a -name '*.[ch]*' | head -1)" ]; then
  mkdir -p $GG; rm -f $GG/*.o $GG/libggpo.a
  for f in $GS/lib/ggpo/*.cpp $GS/lib/ggpo/backends/*.cpp $GS/lib/ggpo/network/*.cpp; do
    case $f in *platform_windows*) continue;; esac
    g++ -O2 -w -std=c++14 -I $GS/include -I $GS/lib/ggpo -c -o $GG/$(basename $f .cpp).o $f &
  done; wait
  ar rcsD $GG/libggpo.a $GG/*.o
fi
# translated code: rebuild only when the generated file is newer than its object
# the game module (sc_game.so): the translated code, module.c and the fsca table
G=$O/game; mkdir -p $G
GF="$CF -DSH4_MODULE -fPIC -w"
for f in build/sweep/all_*.c build/sweep/dispatch.c; do
  o=$G/$(basename $f .c).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ sh4c/sh4rt.h -nt $o ]; then gcc $GF -c -o $o $f & fi
done
gcc $GF -I ~/sc-flycast-m0/core/hw/sh4 -c -o $G/fsca.o sh4c/fsca.c &
gcc $GF -DSH4_TRANSLATOR=\"$TID\" -c -o $G/module.o sh4c/module.c &
rm -f $O/all_*.o $O/dispatch.o $O/fsca.o   # before the module: linked into the executable
for f in m1/m1.c m1/hw.c m1/hle.c m1/threads.c m1/maple.c m1/ta.c m1/tex.c m1/render.c m1/state.c m1/netplay.c m1/aica.c m1/arm7.c m1/config.c m1/online.c m1/artdump.c m1/artswap.c m1/png.c m1/module.c; do
  gcc $CF -Wall $(sdl2-config --cflags) -I m1 -I $GS/include -DSC_BUILD_ID=\"$BID\" -DSC_TRANSLATOR=\"$TID\" -c -o $O/m1_$(basename $f .c).o $f &
done
wait
gcc -shared -o $HOME/m1_build/sc_game.so $G/*.o -lm
g++ -o $HOME/m1_build/m1 $O/*.o $GG/libggpo.a $(sdl2-config --libs) -lGL -lm -ldl -lpthread
ln -sf $HOME/m1_build/m1 /tmp/m1 2>/dev/null; echo built $HOME/m1_build/m1, build $BID
