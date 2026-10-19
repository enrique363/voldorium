"""Merges the per-run art exports of tests/artdump.sh into one list.

Each run folder has screen_<n>.png (the background picture, 640x480), shown_<n>.png (the
screen as the port shows it) and index.csv. Pictures of the same game mode that look alike
(mean difference of a 32x24 thumbnail under 8 of 255) are listed once. Writes art/index.html and
art/list.csv. Usage: python -I tests/art_merge.py build/art
"""
import csv
import os
import struct
import sys
import zlib

MODES = {0: 'game init', 5: 'setup / boot', 9: 'title', 11: 'main menu', 15: 'character select', 17: 'loading (VS)',
         21: 'demo', 23: 'demo fight', 25: 'ranking', 31: 'memory card check', 49: 'options', 53: 'release / credits',
         71: 'opening logos (animated: frames of a zoom, not a still picture)', 61: 'museum: art gallery menu', 39: 'survival result'}


def read_png(path):
    d = open(path, 'rb').read()
    i, idat = 8, b''
    w = h = 0
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h = struct.unpack('>II', c[:8])
        elif t == b'IDAT':
            idat += c
        i += 12 + n
    raw = zlib.decompress(idat)
    st = w * 3 + 1
    return w, h, [raw[y * st + 1:(y + 1) * st] for y in range(h)]


def thumb(path):
    w, h, rows = read_png(path)
    out = []
    for ty in range(24):
        for tx in range(32):
            for k in range(3):
                s = 0
                for y in range(0, 20, 4):
                    row = rows[ty * h // 24 + y * h // 480]
                    for x in range(0, 20, 4):
                        s += row[(tx * w // 32 + x * w // 640) * 3 + k]
                out.append(s // 25)
    return out


def main(root):
    seen, items = [], []
    for run in sorted(os.listdir(root)):
        idx = os.path.join(root, run, 'index.csv')
        if not os.path.isfile(idx):
            continue
        for row in csv.DictReader(open(idx)):
            pic = os.path.join(root, run, row['picture'])
            if not os.path.isfile(pic):
                continue
            th = thumb(pic)
            mode = int(row['game_mode'])
            if any(m == mode and sum(abs(a - b) for a, b in zip(th, t)) < len(th) * 8 for m, t in seen):
                continue
            seen.append((mode, th))
            items.append((run, row['picture'], row['shown'], mode))
    with open(os.path.join(root, 'index.html'), 'w') as f:
        f.write('<!doctype html><meta charset=utf-8><title>4:3 artwork</title><style>body{font:14px system-ui;'
                'background:#181818;color:#ddd;margin:16px}.row{display:flex;flex-wrap:wrap;gap:12px;margin:12px 0;'
                'padding:10px;background:#222;border-radius:8px}figure{margin:0}img{width:420px;max-width:100%;'
                'display:block}figcaption{font-size:12px;color:#aaa;margin-top:4px}h2{font-size:15px;margin:0 0 6px;'
                'width:100%}</style>')
        f.write(f'<h1>4:3 artwork that leaves bars in 16:9</h1><p>{len(items)} screens. Left: the background '
                'picture (640x480; menus, logos and text drawn in front are left out); right: the screen as the '
                'port shows it now. A 16:9 version is 854x480 (or any 16:9 size), with the 640x480 picture in '
                'the middle.</p>\n')
        for i, (run, pic, shown, mode) in enumerate(items, 1):
            f.write(f'<div class=row><h2>{i}. {MODES.get(mode, "game mode " + str(mode))} ({run})</h2>'
                    f'<figure><img src="{run}/{pic}"><figcaption>{run}/{pic}</figcaption></figure>'
                    f'<figure><img src="{run}/{shown}"><figcaption>as shown</figcaption></figure></div>\n')
    try:
        f = open(os.path.join(root, 'list.csv'), 'w', newline='')
    except PermissionError:
        print('list.csv is open in another program (Excel?): not updated')
    else:
      with f:
        w = csv.writer(f)
        w.writerow(['number', 'picture', 'shown', 'game_mode', 'screen', 'found_in_run'])
        for i, (run, pic, shown, mode) in enumerate(items, 1):
            w.writerow([i, f'{run}/{pic}', f'{run}/{shown}', mode, MODES.get(mode, ''), run])
    print(f'{len(items)} distinct screens -> {os.path.join(root, "index.html")}')


if __name__ == '__main__':
    main(sys.argv[1])
