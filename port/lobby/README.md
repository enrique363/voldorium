# Voldorium internet lobby server

`sc_lobby.py` is the lobby, NAT hole punching coordinator and relay for ONLINE VERSUS
(protocol at the top of the file; the game side is `m1/online.c`). Python 3 standard library only.

## Where it runs

| | |
| --- | --- |
| Host | Oracle Cloud Always Free, Canada Southeast (Montreal), `instance-20261018-1855`, VM.Standard.E2.1.Micro (1 GB), Oracle Linux 9 |
| Address | `147.15.149.200` (the game's built-in default, `LOBBY_SERVER` in `m1/online.c`; `[net] lobby = host:port` in `voldorium.ini` overrides it) |
| Ports | UDP 7100 lobby, UDP 7101 relay (Oracle security list "Default Security List for sc-lobby-net" and firewalld) |
| Login | SSH with a key only (no passwords, no root login); the user, host and key file are kept out of the repository, in `local.env` (`SC_LOBBY_HOST`, `SC_LOBBY_KEY`) |
| Service | `sc-lobby.service` (systemd), user `sclobby` (no shell, no home), files in `/opt/sc-lobby` |

## Update notice

The server answers the game's update check (`VERSION`, 1.1.6 on) with the newest release
(`LATEST`). It learns that release from GitHub's releases API (`api.github.com`, outbound HTTPS,
once an hour), so publishing a release on GitHub is enough: the server needs no change per
release. `SC_LATEST=x.y.z` in the service's environment fixes the answer instead. Releases before
1.1.6 never ask; while a newer release exists, the player lists sent to their build keys
(`RELEASE_BUILDS`) start with a made-up entry, UPDATE NOW (those versions can only show a name of
up to 12 letters, as "Challenge UPDATE NOW."), and a challenge to it is declined.

## Usage counts

The log gets a `stats` line every hour (today so far) and at the end of each UTC day:

```
stats 2026-11-09 day from 15:00 UTC: game starts 12 players (1.1.6 3, 1.1.7 9); online versus
5 players (1.1.5 1, 1.1.7 4), 9 visits; matches 3 (direct 2, relay 1)
```

- **game starts**: different players who reached the main menu (the update check, 1.1.6 on; not
  with `update_check = 0`), per version.
- **online versus**: different players who opened ONLINE VERSUS, per version (`other`: a build
  that is no release), and **visits**, every opening.
- **matches**: matches set up, and how they connected.

A player is a hash of its address with a salt made at random each day and kept in memory only:
nothing in the log leads back to an address, and a player can't be followed from one day to the
next. Players behind the same router count as one. A restart starts the counts again (the
line says from when, and a last `until stop` line holds the counts before it).

On the server: `journalctl -u sc-lobby | grep stats`.

## Install or update

From `sc/port` on the PC (Git Bash):

```bash
bash lobby/deploy.sh
```

It copies `sc_lobby.py` and `sc-lobby.service` to the server, installs them and restarts the
service. Logs: `journalctl -u sc-lobby -n 50` on the server.

## Tests

- `python -I tests/lobby_server_test.py`: a local server and two fake players (list, challenge,
  match, relay, a stranger dropped). With a host: `python -I tests/lobby_server_test.py 147.15.149.200`
  tests the real server.
- `bash tests/lobby_inet.sh direct|relay`: two game copies meet through a local server, punch (or
  are forced onto the relay) and play; checksums must match.
