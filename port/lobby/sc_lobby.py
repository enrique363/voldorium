#!/usr/bin/env python3
"""Voldorium: internet lobby, NAT hole punching coordinator and relay (UDP).

Lobby, UDP 7100. Every datagram is one line of printable ASCII, at most 200 bytes:
  game -> server  "SCL1 <build> <id> HELLO <rtt ms> <name>"   presence (every 2 s while in ONLINE VERSUS);
                                                          rtt: its last round trip to the server
                  "SCL1 <build> <id> BYE"
                  "SCL1 <build> <id> TO <peer id> <verb> [<arg>]"   lobby message for another player
                                                          (verbs CHAL, ACCEPT, DECLINE, CANCEL, BUSY)
                  "SCL1 <build> <id> PUNCHED <token> <0|1>"   hole punching result
  server -> game  "SCL1 LIST <id>,<name>,<ip>:<port>,<rtt> ..."  the other players of the same build
                  "SCL1 FROM <peer id> <ip>:<port> <verb> [<arg>]"
                  "SCL1 MATCH <token> <peer ip>:<port>"    after an ACCEPT, to both players: punch now
                  "SCL1 GO <token> DIRECT <peer ip>:<port>" or "SCL1 GO <token> RELAY <relay port>"
The address the server sees for a player is its public (outside the router) address of the game
port; the game uses that same port for punching and for GGPO.

Relay, UDP 7101. A player registers with "SCR1 <token> <id>" from its game port; once both
players of a match have registered, any other datagram from one is forwarded to the other.
Nothing else is forwarded: unknown senders are dropped.

Privacy: the log holds ids and match tokens only, no names or addresses.
Protections: printable ASCII and length checks on every lobby datagram, a token bucket per
source address, caps on players, matches and relays, relay rate and size limits, idle expiry.
The server keeps no files and runs nothing it receives. Standard library only (Python 3.6+).
"""
import os
import re
import secrets
import selectors
import socket
import sys
import time

LOBBY_PORT = int(os.environ.get('SC_LOBBY_PORT', '7100'))
RELAY_PORT = LOBBY_PORT + 1
MAX_PLAYERS = 500
MAX_LIST = 12                 # players per LIST reply (one datagram)
PLAYER_TIMEOUT = 8.0          # seconds without HELLO
MATCH_TIMEOUT = 15.0          # punch phase
MAX_MATCHES = 200
RELAY_IDLE = 60.0             # seconds without traffic
RELAY_MAX_AGE = 4 * 3600.0
MAX_RELAYS = 100
RELAY_PPS = 400               # packets per second per relay (GGPO sends far fewer)
RELAY_BPS = 200_000           # bytes per second per relay
RELAY_MAX_PACKET = 1400
RATE = 30.0                   # lobby datagrams per second per source address
BURST = 60.0

TOKEN_RE = re.compile(r'^[0-9a-f]{16}$')
ID_RE = re.compile(r'^[0-9a-f]{8}$')
BUILD_RE = re.compile(r'^[0-9A-Za-z]{1,16}$')
NAME_RE = re.compile(r'^[A-Z0-9 -]{1,12}$')
VERBS = {'CHAL', 'ACCEPT', 'DECLINE', 'CANCEL', 'BUSY'}


def log(*a):
    print(time.strftime('%Y-%m-%d %H:%M:%S'), *a, flush=True)


def addr_str(a):
    return '%s:%d' % a


class Bucket:
    __slots__ = ('tokens', 'last')

    def __init__(self):
        self.tokens = BURST
        self.last = time.monotonic()

    def take(self):
        now = time.monotonic()
        self.tokens = min(BURST, self.tokens + (now - self.last) * RATE)
        self.last = now
        if self.tokens < 1:
            return False
        self.tokens -= 1
        return True


class Player:
    __slots__ = ('id', 'build', 'name', 'addr', 'seen', 'rtt')

    def __init__(self, pid, build, name, addr):
        self.id, self.build, self.name, self.addr, self.seen = pid, build, name, addr, time.monotonic()
        self.rtt = 0


class Match:
    __slots__ = ('token', 'a', 'b', 'addr', 'result', 'created', 'done')

    def __init__(self, token, a, b):
        self.token, self.a, self.b = token, a, b          # player ids, a = challenger
        self.addr = {}                                    # id -> public address at match time
        self.result = {}                                  # id -> punched 0/1
        self.created = time.monotonic()
        self.done = False


class Relay:
    __slots__ = ('token', 'ids', 'addrs', 'last', 'created', 'window', 'pkts', 'bytes')

    def __init__(self, token, ids):
        self.token, self.ids = token, ids
        self.addrs = {}                                   # id -> address registered on the relay port
        self.last = self.created = time.monotonic()
        self.window, self.pkts, self.bytes = int(self.created), 0, 0


class Server:
    def __init__(self):
        self.lobby = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.lobby.bind(('0.0.0.0', LOBBY_PORT))
        self.lobby.setblocking(False)
        self.relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.relay.bind(('0.0.0.0', RELAY_PORT))
        self.relay.setblocking(False)
        self.players = {}        # id -> Player
        self.buckets = {}        # ip -> Bucket
        self.matches = {}        # token -> Match
        self.relays = {}         # token -> Relay
        self.relay_by_addr = {}  # address -> Relay
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.lobby, selectors.EVENT_READ, self.on_lobby)
        self.sel.register(self.relay, selectors.EVENT_READ, self.on_relay)
        self.last_sweep = time.monotonic()
        log('lobby on UDP %d, relay on UDP %d' % (LOBBY_PORT, RELAY_PORT))

    # ---- lobby ----
    def send(self, addr, text):
        try:
            self.lobby.sendto(text.encode('ascii'), addr)
        except OSError:
            pass

    def on_lobby(self):
        for _ in range(256):
            try:
                data, addr = self.lobby.recvfrom(512)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                return
            b = self.buckets.get(addr[0])
            if b is None:
                if len(self.buckets) > 20000:
                    continue
                b = self.buckets[addr[0]] = Bucket()
            if not b.take() or len(data) > 200:
                continue
            try:
                text = data.decode('ascii')
            except UnicodeDecodeError:
                continue
            if not text.isprintable():
                continue
            self.handle(text.split(' '), addr)

    def handle(self, w, addr):
        if len(w) < 4 or w[0] != 'SCL1' or not BUILD_RE.match(w[1]) or not ID_RE.match(w[2]):
            return
        build, pid, verb = w[1], w[2], w[3]
        p = self.players.get(pid)
        if p is not None and (p.addr != addr or p.build != build):
            if verb != 'HELLO':
                return            # only the registered address may speak for an id
        if verb == 'HELLO':
            rtt = 0
            if len(w) > 5 and w[4].isdigit() and len(w[4]) <= 4:
                rtt, w = int(w[4]), w[:4] + w[5:]
            name = ' '.join(w[4:])[:12]
            if not NAME_RE.match(name):
                return
            if p is None:
                if len(self.players) >= MAX_PLAYERS:
                    return
                p = self.players[pid] = Player(pid, build, name, addr)
                log('join %s' % pid)    # no names or addresses in the log
            p.name, p.addr, p.build, p.seen, p.rtt = name, addr, build, time.monotonic(), rtt
            self.send_list(p)
        elif verb == 'BYE' and p is not None:
            del self.players[pid]
            log('leave %s' % pid)
        elif verb == 'TO' and p is not None and len(w) >= 6:
            target, msg = w[4], w[5]
            arg = ' '.join(w[6:])[:12]
            t = self.players.get(target)
            if not ID_RE.match(target) or msg not in VERBS or t is None or t.build != p.build or target == pid:
                return
            if arg and not NAME_RE.match(arg):
                return
            p.seen = time.monotonic()
            self.send(t.addr, 'SCL1 FROM %s %s %s%s' % (pid, addr_str(p.addr), msg, ' ' + arg if arg else ''))
            if msg == 'ACCEPT':
                self.start_match(challenger=t, accepter=p)
        elif verb == 'PUNCHED' and p is not None and len(w) == 6 and TOKEN_RE.match(w[4]) and w[5] in ('0', '1'):
            m = self.matches.get(w[4])
            if m is not None and pid in (m.a, m.b) and not m.done:
                m.result[pid] = w[5] == '1'
                if len(m.result) == 2:
                    self.finish_match(m)

    def send_list(self, p):
        others = [o for o in self.players.values() if o.build == p.build and o.id != p.id]
        others.sort(key=lambda o: o.seen, reverse=True)
        items = ['%s,%s,%s,%d' % (o.id, o.name.replace(' ', '_'), addr_str(o.addr), o.rtt) for o in others[:MAX_LIST]]
        self.send(p.addr, 'SCL1 LIST ' + ' '.join(items))

    def start_match(self, challenger, accepter):
        if len(self.matches) >= MAX_MATCHES:
            return
        token = secrets.token_hex(8)
        m = Match(token, challenger.id, accepter.id)
        m.addr = {challenger.id: challenger.addr, accepter.id: accepter.addr}
        self.matches[token] = m
        self.send(challenger.addr, 'SCL1 MATCH %s %s' % (token, addr_str(accepter.addr)))
        self.send(accepter.addr, 'SCL1 MATCH %s %s' % (token, addr_str(challenger.addr)))
        log('match %s %s vs %s' % (token, challenger.id, accepter.id))

    def finish_match(self, m):
        m.done = True
        a, b = m.addr[m.a], m.addr[m.b]
        if m.result.get(m.a) and m.result.get(m.b):
            self.send(a, 'SCL1 GO %s DIRECT %s' % (m.token, addr_str(b)))
            self.send(b, 'SCL1 GO %s DIRECT %s' % (m.token, addr_str(a)))
            log('match %s direct' % m.token)
        else:
            if len(self.relays) >= MAX_RELAYS:
                log('match %s: relay full' % m.token)
                return
            self.relays[m.token] = Relay(m.token, (m.a, m.b))
            for x in (a, b):
                self.send(x, 'SCL1 GO %s RELAY %d' % (m.token, RELAY_PORT))
            log('match %s relay' % m.token)

    # ---- relay ----
    def on_relay(self):
        for _ in range(512):
            try:
                data, addr = self.relay.recvfrom(2048)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                return
            r = self.relay_by_addr.get(addr)
            if r is None:
                self.register(data, addr)
                continue
            now = time.monotonic()
            if int(now) != r.window:
                r.window, r.pkts, r.bytes = int(now), 0, 0
            r.pkts += 1
            r.bytes += len(data)
            if r.pkts > RELAY_PPS or r.bytes > RELAY_BPS or len(data) > RELAY_MAX_PACKET:
                continue
            if data.startswith(b'SCR1 '):
                continue          # repeated registration
            other = [x for i, x in r.addrs.items() if x != addr]
            if other:
                r.last = now
                try:
                    self.relay.sendto(data, other[0])
                except OSError:
                    pass

    def register(self, data, addr):
        if len(data) > 64 or not data.startswith(b'SCR1 '):
            return
        try:
            w = data.decode('ascii').split(' ')
        except UnicodeDecodeError:
            return
        if len(w) != 3 or not TOKEN_RE.match(w[1]) or not ID_RE.match(w[2]):
            return
        r = self.relays.get(w[1])
        if r is None or w[2] not in r.ids or w[2] in r.addrs:
            return
        r.addrs[w[2]] = addr
        self.relay_by_addr[addr] = r
        r.last = time.monotonic()
        log('relay %s: %s registered' % (r.token, w[2]))

    # ---- housekeeping ----
    def sweep(self):
        now = time.monotonic()
        for pid in [k for k, p in self.players.items() if now - p.seen > PLAYER_TIMEOUT]:
            del self.players[pid]
        for t in [k for k, m in self.matches.items() if now - m.created > MATCH_TIMEOUT]:
            m = self.matches.pop(t)
            if not m.done:
                log('match %s timed out (%d reports)' % (t, len(m.result)))
                if m.result:
                    self.finish_match(m)     # one side reported: the other gets relay too
        for t in [k for k, r in self.relays.items()
                  if now - r.last > RELAY_IDLE or now - r.created > RELAY_MAX_AGE]:
            r = self.relays.pop(t)
            for a in r.addrs.values():
                self.relay_by_addr.pop(a, None)
            log('relay %s closed' % t)
        if len(self.buckets) > 5000:
            self.buckets = {ip: b for ip, b in self.buckets.items() if now - b.last < 60}

    def run(self):
        while True:
            for key, _ in self.sel.select(timeout=1.0):
                key.data()
            if time.monotonic() - self.last_sweep >= 1.0:
                self.last_sweep = time.monotonic()
                self.sweep()


if __name__ == '__main__':
    try:
        Server().run()
    except KeyboardInterrupt:
        sys.exit(0)
