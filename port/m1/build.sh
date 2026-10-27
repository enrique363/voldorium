#!/usr/bin/env bash
# Builds the M1 executable (WSL): every translated function (build/sweep) + runtime + HLE.
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
# translated code: rebuild only when the generated file is newer than its object
# the game module (sc_game.so): the translated code, module.c and the fsca table
G=$O/game; mkdir -p $G
GF="$CF -DSH4_MODULE -fPIC -w"
for f in build/sweep/all_*.c build/sweep/dispatch.c; do
  o=$G/$(basename $f .c).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ sh4c/sh4rt.h -nt $o ]; then gcc $GF -c -o $o $f & fi
done
gcc $GF -I ~/sc-flycast-m0/core/hw/sh4 -c -o $G/fsca.o sh4c/fsca.c &
gcc $GF -c -o $G/module.o sh4c/module.c &
rm -f $O/all_*.o $O/dispatch.o $O/fsca.o   # before the module: linked into the executable
for f in m1/m1.c m1/hw.c m1/hle.c m1/threads.c m1/maple.c m1/ta.c m1/tex.c m1/render.c m1/state.c m1/netplay.c m1/aica.c m1/arm7.c m1/config.c m1/online.c m1/artdump.c m1/artswap.c m1/png.c m1/module.c; do gcc $CF -Wall $(sdl2-config --cflags) -I m1 -c -o $O/m1_$(basename $f .c).o $f & done
wait
gcc -shared -o $HOME/m1_build/sc_game.so $G/*.o -lm
gcc -o $HOME/m1_build/m1 $O/*.o $(sdl2-config --libs) -lGL -lm -ldl
ln -sf $HOME/m1_build/m1 /tmp/m1 2>/dev/null; echo built $HOME/m1_build/m1
