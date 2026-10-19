"""Exercises the lobby server (lobby/sc_lobby.py) with two fake players over UDP.

Usage: python -I tests/lobby_server_test.py [host [port]]   (default: starts a local server on
port 7200 and tests that; with a host, tests that server). Checks: both players list each other,
a challenge reaches the other side, ACCEPT starts a match, failed punching gives a relay, and the
relay forwards between the two registered addresses only; malformed input is ignored.
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('0.0.0.0', 0))
    s.settimeout(3)
    return s


def expect(s, prefix):
    end = time.time() + 5
    while time.time() < end:
        try:
            data, _ = s.recvfrom(2048)
        except socket.timeout:
            break
        if data.decode('ascii', 'replace').startswith(prefix):
            return data.decode('ascii', 'replace')
    raise SystemExit('FAIL: no "%s"' % prefix)


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else None
    port = int(sys.argv[2]) if len(sys.argv) > 2 else (7100 if host else 7200)
    server = None
    if not host:
        env = dict(os.environ, SC_LOBBY_PORT=str(port))
        server = subprocess.Popen([sys.executable, '-I', os.path.join(HERE, '..', 'lobby', 'sc_lobby.py')], env=env)
        time.sleep(1)
        host = '127.0.0.1'
    lobby, relay = (host, port), (host, port + 1)
    a, b = sock(), sock()
    try:
        a.sendto(b'garbage \x00\xff', lobby)
        a.sendto(b'SCL1 test aaaaaaaa HELLO 25 ALICE', lobby)
        b.sendto(b'SCL1 test bbbbbbbb HELLO 40 BOB', lobby)
        expect(b, 'SCL1 LIST aaaaaaaa,ALICE,')
        a.sendto(b'SCL1 test aaaaaaaa HELLO 25 ALICE', lobby)
        print('list ok:', expect(a, 'SCL1 LIST bbbbbbbb,BOB,'))
        a.sendto(b'SCL1 test aaaaaaaa TO bbbbbbbb CHAL ALICE', lobby)
        print('challenge ok:', expect(b, 'SCL1 FROM aaaaaaaa'))
        b.sendto(b'SCL1 test bbbbbbbb TO aaaaaaaa ACCEPT', lobby)
        expect(a, 'SCL1 FROM bbbbbbbb')
        token = expect(a, 'SCL1 MATCH').split()[2]
        expect(b, 'SCL1 MATCH ' + token)
        print('match ok:', token)
        for s, pid in ((a, 'aaaaaaaa'), (b, 'bbbbbbbb')):
            s.sendto(('SCL1 test %s PUNCHED %s 0' % (pid, token)).encode(), lobby)
        go = expect(a, 'SCL1 GO')
        expect(b, 'SCL1 GO')
        assert go.split()[3] == 'RELAY', go
        print('relay chosen ok')
        a.sendto(('SCR1 %s aaaaaaaa' % token).encode(), relay)
        b.sendto(('SCR1 %s bbbbbbbb' % token).encode(), relay)
        time.sleep(0.3)
        a.sendto(b'ggpo-from-a', relay)
        data, _ = b.recvfrom(2048)
        assert data == b'ggpo-from-a', data
        b.sendto(b'ggpo-from-b', relay)
        data, _ = a.recvfrom(2048)
        assert data == b'ggpo-from-b', data
        print('relay forwards ok')
        c = sock()                     # a stranger cannot inject into the relay
        c.sendto(b'evil', relay)
        b.settimeout(1)
        try:
            data, _ = b.recvfrom(2048)
            raise SystemExit('FAIL: stranger got through: %r' % data)
        except socket.timeout:
            print('stranger dropped ok')
        print('PASS')
    finally:
        if server:
            server.terminate()


if __name__ == '__main__':
    main()
