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
(native or a 16:9 size up to your monitor's), anti-aliasing (off, 2x, 4x, 8x), VSync (see
below) and loading (ORIGINAL: the disc loads as fast as the Dreamcast's drive, as on the
console; FAST: instantly, so the VS screen before a fight is cut short; online matches always
load at the original speed); changes apply at once and are saved in voldorium.ini. Alt+Enter or F11: full screen on/off. Esc: quit (during an online match: leave it and go back to the main menu; if a match is cut off, both players return there).

VSync
-----
VSync makes the game wait for your monitor to finish showing one picture before it sends the
next, so you never see two half pictures at once (screen tearing: a horizontal line where the
image is split, most visible when the camera moves). The cost is a short wait for the next
screen refresh. Voldorium uses adaptive VSync where the graphics driver offers it: a picture
that is late shows at once instead of waiting a whole refresh, so online matches never slow down.

Recommended: leave VSync ON (the default) on every monitor.
- G-Sync / FreeSync monitor: the game runs at 60 frames a second, below the monitor's maximum,
  so the monitor shows each picture as soon as it is ready. VSync then adds no delay at all and
  only catches the cases where G-Sync / FreeSync is not active. For G-Sync in this game (it
  runs as a borderless full screen window), choose "Enable for windowed and full screen mode"
  in the NVIDIA Control Panel (Set up G-SYNC), or turn on Variable refresh rate in Windows
  (Settings > Display > Graphics). With it working, the monitor's own on-screen display shows
  about 60 Hz while you play.
- 120 Hz and faster monitors without G-Sync / FreeSync: VSync on costs at most one short
  refresh (about 3 ms on average at 180 Hz).
- 60 Hz monitors without G-Sync / FreeSync: VSync on adds about half a frame (around 8 ms) on
  average. Turn it OFF only if you want the lowest possible input delay and do not mind tearing.

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

Steam Deck
----------
Copy this whole folder and your disc dump to the Deck. In Desktop Mode, Steam > Games > Add a
Non-Steam Game to My Library > Voldorium.exe; then in its Properties > Compatibility, force a
recent Proton. The first start asks for the disc dump. It is the same program as on Windows, so
Online Versus works between Deck and Windows players with the same build. The 1280x800 screen
shows the game in 16:9 with thin bars above and below.

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

HD art: the textures folder holds new, high resolution art that the game's textures are drawn
with instead: the HIGH / MID / S-MID / LOW and "!" labels shown at a hit in Practice
(textures/replace.txt lists each piece; delete the folder for the original pictures).

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
