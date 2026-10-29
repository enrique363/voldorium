#!/bin/sh
# Voldorium's first start on Linux (m1/module.c runs this, in the builder folder next to
# voldorium): builds the game module sc_game.so from the player's own disc dump, into the
# Voldorium folder. Usage: first_run.sh <disc.gdi> <Voldorium folder>. Everything it downloads
# (the compiler and two Flycast files) goes into toolchain/ here, at pinned versions checked by
# SHA-256. Nothing is installed on the system. Writes work/first_run.pid while it runs and
# work/first_run.result (ok or failed) at the end, for the game to wait on.
cd "$(dirname "$0")" || exit 1
mkdir -p work
rm -f work/first_run.result
echo $$ > work/first_run.pid
printf '\033]0;Voldorium: building the game from your disc\007'
echo "Voldorium: building the game from your disc"
echo "============================================"
echo "The game's code is translated from YOUR OWN disc dump, on this PC. Nothing of the game is"
echo "downloaded. This takes a few minutes, once."
echo

finish() {
  echo "$1" > work/first_run.result
  rm -f work/first_run.pid
  if [ "$1" = ok ]; then
    echo
    echo "Finished: Voldorium starts now."
    exit 0
  fi
  echo
  echo "The game was not built. The message above says why; README.txt's Troubleshooting section"
  echo "lists the usual causes. Start Voldorium again to retry: finished steps are not redone."
  [ -t 0 ] && { printf "Press Enter to close this window. "; read -r _; }
  exit 1
}

if [ -z "$2" ]; then
  echo "usage: first_run.sh disc.gdi VoldoriumFolder"
  finish failed
fi
PY=""
for p in python3 python; do
  if command -v $p > /dev/null 2>&1 && $p -c "import sys, lzma; sys.exit(0 if sys.version_info >= (3, 8) else 1)" > /dev/null 2>&1; then
    PY=$p; break
  fi
done
if [ -z "$PY" ]; then
  echo "Python 3 was not found. Install it with your system's package manager (for example"
  echo "sudo apt install python3, sudo dnf install python3, sudo pacman -S python) and start"
  echo "Voldorium again."
  finish failed
fi
"$PY" -I tools/build.py --download --only module --gdi "$1" --module-dir "$2" --out "$PWD/work" && finish ok
finish failed
