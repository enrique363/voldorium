#!/usr/bin/env bash
# Assembles the player folder sc/port/build/player (Windows): Voldorium.exe (GUI), m1.exe
# (console, same program), SDL2.dll, the rollback range table, the BIOS font when this machine
# has it, a starting voldorium.ini and README.txt. No game data goes in: on its first start the
# game asks for the player's own disc dump (.gdi) with a file picker.
# Usage (Git Bash, from sc/port, after m1/build_win.sh): bash m1/make_player.sh
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=$(cygpath -u "$LOCALAPPDATA")/sc-m1
DST=build/player
mkdir -p $DST
cp -u $BIN/Voldorium.exe $BIN/m1.exe $BIN/SDL2.dll $DST/
# before the rename: the old executable goes, the old settings file becomes voldorium.ini
rm -f $DST/Soulcalibur.exe
[ -f $DST/sc-port.ini ] && [ ! -f $DST/voldorium.ini ] && mv $DST/sc-port.ini $DST/voldorium.ini
cp -u ../ggpo/savestate_ranges.tsv $DST/
# 16:9 replacement pictures: the fingerprints of the known 4:3 backgrounds (tests/art_refs.py);
# the player's own <name>.png files go next to them and are kept
mkdir -p $DST/art16x9; [ -f art16x9/refs.txt ] && cp -u art16x9/refs.txt $DST/art16x9/    # made from your own dump, not in git
# Flycast's replacement BIOS font (system text such as the memory card messages); optional
[ -f $BIN/biosfont.bin ] && cp -u $BIN/biosfont.bin $DST/
if [ ! -f $DST/voldorium.ini ]; then
  DISC=$(cygpath -w "$(pwd)/../disc/gdi/soulcalibur.gdi")
  cat > $DST/voldorium.ini <<EOF
; Voldorium settings (see README.txt)

[game]
disc = $DISC
font = biosfont.bin

[video]
fullscreen = 1
resolution = native
window = 1280x720
msaa = 4
backgrounds = stretch
text = smooth
vsync = 1

[audio]
enabled = 1
volume = 100

[keys]
up = Up
down = Down
left = Left
right = Right
a = Z
b = X
x = A
y = S
start = Return
l = Q
r = W

[net]
port = 7000
peer =
player = 1
delay = 2
menu_delay = 8
EOF
fi
cat > $DST/README.txt <<'EOF'
Voldorium
=========

A native Windows engine for the Dreamcast fighting game SoulCalibur (USA). It contains no game
data: it runs your own disc dump.

Start Voldorium.exe. The first time, it asks for your disc dump (the .gdi file of your own
disc, with its track files next to it); after that it starts straight away.
Everything else is in the game's own menus. To choose another disc dump, delete the "disc ="
line in voldorium.ini.

Controls: arrow keys = D-pad, Z = A, X = B, A = X, S = Y, Q / W = L / R triggers,
Enter = Start (change them in voldorium.ini, [keys], with SDL key names). Confirm with A (Z,
or the controller's A button): Start (Enter) only works in the main menu; B (X) goes back. A
game controller plays player 1 with the keyboard (left stick or D-pad; triggers or shoulder
buttons for L / R); a second controller is player 2, for VS BATTLE. In the gallery's picture
viewer L / R change pictures (the D-pad moves the picture).
The game starts full screen at your monitor's resolution. GRAPHICS in the main menu (under
OPTIONS) switches between full screen and a window, sets the resolution the game is drawn at
(native or a 16:9 size up to your monitor's), anti-aliasing (off, 2x, 4x, 8x) and VSync (on:
no screen tearing; off: the lowest input delay; G-Sync / FreeSync work with it on in full
screen); changes apply
at once and are saved in voldorium.ini. Alt+Enter or F11: full screen on/off. Esc: quit (during an online match: leave it and go back to the main menu; if a match is cut off, both players return there).

Online Versus (same network)
----------------------------
Choose ONLINE VERSUS in the main menu (under VS BATTLE). Other players appear as OPPONENT:
nothing about your PC (not even its name) is shown to them. To show a nickname instead, add
"name = YOURNICK" under [net] in voldorium.ini. The lobby lists every player on
your network who is in the same lobby with the same build of the game. Pick a name to
challenge that player; they choose ACCEPT or DECLINE. The game then restarts both copies
straight into VS BATTLE character select, connected (rollback in fights). No ports to set.
The first time, Windows may ask whether Voldorium may use the network: allow it (private
networks), or the lobby stays empty.
Players on other networks (over the internet) appear in the same list, through the lobby
server: still no ports to set. If your router blocks a direct connection, the match goes
through the server's relay automatically.

Memory card
-----------
The game has a memory card (VMU) in controller port A: vmu_a1.bin next to the executable holds
your saves (options, unlocked characters, records). The first start creates it. Online matches
run without a card, so both players start alike. A save of your own from a real card or an
emulator in Nexus .dci format can be copied in with tools/vmu_import.py from the source
repository (python tools/vmu_import.py vmu_a1.bin yoursave.dci; the old card is kept as .bak).

16:9 pictures
-------------
Menus and loading screens whose artwork is 4:3: voldorium.ini [video] backgrounds chooses how
they fill 16:9. stretch (default): the picture is stretched to full width. blur: softly
blurred sides. replace: a 16:9 version replaces the picture where there is one (blurred sides
elsewhere): put <name>.png in the art16x9 folder (names in art16x9/refs.txt, any 16:9 size,
the original picture centred at the same scale).

Text: menu and HUD text is drawn from upscaled copies of the game's font textures, so it keeps
its size with smooth edges at high resolutions. [video] text = sharp draws it as the original
pixels instead.

Manual connection (GGPO rollback, any network)
----------------------------------------------
Without the lobby, two players can also connect from a command prompt in this folder, both at
about the same time (the session begins at boot):
  Voldorium.exe --join <other player's address>:7000 --port 7000 --player 1
(the other player uses --player 2 and the first player's address). Each player opens UDP port
7000 in their router / firewall. Menus run in lockstep ("menu_delay" in voldorium.ini sets their
input delay, raise it on long distances); fights use rollback with --delay frames (default 2).

m1_log.txt (next to the executable) is the log. During an online session it has a line every
60 frames like "net: frame 1250 checksum 1a2b3c..."; the two players' lines for the same frame
must match. If they do not, keep both logs: that is a desync report.

Files: voldorium.ini (settings), savestate_ranges.tsv (what rollback saves), biosfont.bin
(optional system font). No game data is included.
EOF
echo "player folder: $DST"
ls -la $DST
