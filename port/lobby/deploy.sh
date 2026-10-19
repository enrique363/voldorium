#!/usr/bin/env bash
# Installs or updates the lobby server on the Oracle Cloud instance (lobby/README.md).
# Usage (Git Bash, from sc/port): bash lobby/deploy.sh [user@host]
# The login is not in git: SC_LOBBY_HOST (user@host) and SC_LOBBY_KEY (the SSH key file) come
# from local.env at the repository root, or from the environment.
set -euo pipefail
cd "$(dirname "$0")"
[ -f ../../local.env ] && . ../../local.env
DEST=${1:-${SC_LOBBY_HOST:?set SC_LOBBY_HOST=user@host in local.env, or pass it}}
KEY=${SC_LOBBY_KEY:?set SC_LOBBY_KEY=path/to/ssh_key in local.env}
SSH="ssh -i $KEY -o ConnectTimeout=20"
scp -i "$KEY" -q sc_lobby.py sc-lobby.service "$DEST:/tmp/"
$SSH "$DEST" 'set -e
sudo install -d -o root -g root -m 755 /opt/sc-lobby
sudo install -o root -g root -m 644 /tmp/sc_lobby.py /opt/sc-lobby/sc_lobby.py
sudo install -o root -g root -m 644 /tmp/sc-lobby.service /etc/systemd/system/sc-lobby.service
rm -f /tmp/sc_lobby.py /tmp/sc-lobby.service
sudo restorecon -R /opt/sc-lobby /etc/systemd/system/sc-lobby.service 2>/dev/null || true
sudo systemctl daemon-reload
sudo systemctl enable sc-lobby >/dev/null 2>&1
sudo systemctl restart sc-lobby
sleep 1
systemctl is-active sc-lobby
sudo journalctl -u sc-lobby -n 3 --no-pager' 2>&1 | grep -v "post-quantum\|store now\|openssh.com/pq"
