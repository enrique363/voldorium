"""Extracts the boot files from your own disc dump, for building Voldorium.

Reads the .gdi of a Dreamcast GD-ROM dump (the high-density data track at LBA 45000, raw
2352-byte sectors) and writes extracted/IP.BIN (its first 16 sectors) and extracted/1ST_READ.BIN
(found through the ISO 9660 directory) next to this repository's tools/ folder. The translator
(port/sh4c/sweep.py) turns 1ST_READ.BIN into C. Both files are checked against the SHA-1 of the
supported release, SoulCalibur USA (T-1401N, V1.000); nothing of the game is stored here.

Usage: python -I tools/extract_boot.py path/to/game.gdi
"""
import hashlib
import os
import sys

SUPPORTED = {
    '1ST_READ.BIN': '967ca1fe2e8c7df57a5de1e83d939bed483cc6c9',
    'IP.BIN': '38b8ef1fc65f53ccf154038d16e26a99ecc65875',
}
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'extracted')


def data_track(gdi):
    folder = os.path.dirname(os.path.abspath(gdi))
    for line in open(gdi):
        f = line.split()
        if len(f) >= 5 and f[0].isdigit() and int(f[1]) >= 45000 and f[2] == '4' and f[3] == '2352':
            return os.path.join(folder, f[4].strip('"')), int(f[1])
    raise SystemExit('%s: no high-density data track (LBA 45000, 2352-byte sectors)' % gdi)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    path, base = data_track(sys.argv[1])
    track = open(path, 'rb')

    def read(lba, count):
        out = b''
        for i in range(count):
            track.seek((lba + i - base) * 2352 + 16)    # mode 1: 16 header bytes, then 2048 of data
            out += track.read(2048)
        return out

    files = {'IP.BIN': read(45000, 16)}
    pvd = read(45000 + 16, 1)
    if pvd[1:6] != b'CD001':
        raise SystemExit('no ISO 9660 volume on the data track')
    root = int.from_bytes(pvd[156 + 2:156 + 6], 'little')
    root_len = int.from_bytes(pvd[156 + 10:156 + 14], 'little')
    for s in range((root_len + 2047) // 2048):
        d = read(root + s, 1)
        o = 0
        while o < 2048 and d[o]:
            n = d[o + 32]
            if d[o + 33:o + 33 + n].split(b';')[0] == b'1ST_READ.BIN':
                ext = int.from_bytes(d[o + 2:o + 6], 'little')
                size = int.from_bytes(d[o + 10:o + 14], 'little')
                files['1ST_READ.BIN'] = read(ext, (size + 2047) // 2048)[:size]
            o += d[o]
    if '1ST_READ.BIN' not in files:
        raise SystemExit('1ST_READ.BIN not found on the disc')
    os.makedirs(OUT, exist_ok=True)
    ok = True
    for name, data in files.items():
        digest = hashlib.sha1(data).hexdigest()
        match = digest == SUPPORTED[name]
        ok &= match
        open(os.path.join(OUT, name), 'wb').write(data)
        print('%-13s %8d bytes  sha1 %s  %s' % (name, len(data), digest, 'ok' if match else 'NOT the supported release'))
    if not ok:
        raise SystemExit('This dump is not SoulCalibur USA (T-1401N, V1.000); the translation tables are made for it.')
    print('extracted to', OUT)


if __name__ == '__main__':
    main()
