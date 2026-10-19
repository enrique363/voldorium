# Voldorium internet lobby server

`sc_lobby.py` is the lobby, NAT hole punching coordinator and relay for ONLINE VERSUS
(protocol at the top of the file; the game side is `m1/online.c`). Python 3 standard library only.

## Where it runs

| | |
| --- | --- |
| Host | Oracle Cloud Always Free, Canada Southeast (Montreal), `instance-20261018-1855`, VM.Standard.E2.1.Micro (1 GB), Oracle Linux 9 |
| Address | `147.15.149.200` (the game's built-in default, `LOBBY_SERVER` in `m1/online.c`; `[net] lobby = host:port` in `voldorium.ini` overrides it) |
| Ports | UDP 7100 lobby, UDP 7101 relay (Oracle security list "Default Security List for sc-lobby-net" and firewalld) |
| Login | `ssh -i ~/.ssh/sc_lobby opc@147.15.149.200` (key only: no passwords, no root login) |
| Service | `sc-lobby.service` (systemd), user `sclobby` (no shell, no home), files in `/opt/sc-lobby` |

## Install or update

From `sc/port` on the PC (Git Bash):

```bash
bash lobby/deploy.sh
```

It copies `sc_lobby.py` and `sc-lobby.service` to the server, installs them and restarts the
service. Logs: `ssh -i ~/.ssh/sc_lobby opc@147.15.149.200 journalctl -u sc-lobby -n 50`.

## Tests

- `python -I tests/lobby_server_test.py`: a local server and two fake players (list, challenge,
  match, relay, a stranger dropped). With a host: `python -I tests/lobby_server_test.py 147.15.149.200`
  tests the real server.
- `bash tests/lobby_inet.sh direct|relay`: two game copies meet through a local server, punch (or
  are forced onto the relay) and play; checksums must match.
