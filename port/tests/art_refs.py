"""Writes the fingerprints of the exported 4:3 backgrounds for the 16:9 replacements.

For each <name>.png (640x480) in the source folder (build/art_4x3 by default), one line
"<name> <hex>" with its 32x24 RGB fingerprint: the mean of each 20x20 cell, as the game takes it
(m1/artdump.c art_fingerprint). The game matches the background it draws against these and, when
<name>.png exists in the replacement folder, draws that instead (m1/artswap.c).
Usage: python -I tests/art_refs.py [build/art_4x3] [art16x9/refs.txt]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from art_merge import read_png  # noqa: E402


def fingerprint(path):
    w, h, rows = read_png(path)
    if (w, h) != (640, 480):
        raise SystemExit(f'{path}: {w}x{h}, expected 640x480 (an exported 4:3 picture)')
    out = bytearray()
    for ty in range(24):
        for tx in range(32):
            for k in range(3):
                s = 0
                for y in range(20):
                    row = rows[ty * 20 + y]
                    for x in range(20):
                        s += row[(tx * 20 + x) * 3 + k]
                out.append(s // 400)
    return out.hex()


def main(src, dst):
    names = sorted(f[:-4] for f in os.listdir(src) if f.endswith('.png'))
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    with open(dst, 'w', newline='\n') as f:
        f.write('# 4:3 backgrounds the game recognises: name, 32x24 RGB fingerprint (tests/art_refs.py).\n'
                '# A 16:9 replacement is <name>.png in this folder.\n')
        for n in names:
            f.write(f'{n} {fingerprint(os.path.join(src, n + ".png"))}\n')
    print(f'{len(names)} fingerprints -> {dst}')


if __name__ == '__main__':
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'build', 'art_4x3'),
         sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, 'art16x9', 'refs.txt'))
