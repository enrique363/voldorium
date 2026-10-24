"""Draws the hit level labels (High, Mid, S-Mid, Low, !) shown at a hit in Practice, at 4 times
the size of the game's own, as replacement art for port/m1/tex.c (port/m1/textures).

The labels share one 4-bit paletted texture with the hit sparks (256 x 256 texels; the labels
are its 64 x 64 cells at rows 128-255), and the game colours it with several palettes (light
blue, deep blue, yellow). So the art is written in palette terms: each pixel names two palette
entries (red, green), the mix between them (blue) and its coverage (alpha), and tex.c turns it
into colours with whichever palette the game picks. The palette, as the game sets it:
  1-4   greys, dark to light (outlines, letter shading)
  5-6   gold, dark and light (the "!")
  7-15  the bolt's colour ramp, dark to white (the colour the palettes change)

Every cell is square, as in the game, at exactly 4 times its texels, so nothing is stretched.
Needs Pillow and numpy, and the Bookman Old Style fonts (Windows; --font to use others).

Usage: python tools/hitlabels.py [--preview file.png]
"""
import argparse
import math
import os
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

K = 4                   # art pixels per texel
SS = 4                  # supersampling while drawing
CELL = 64 * K           # one cell of art
HALF_WIDTH = 8.5        # the bolt's half width in the middle, texels
HASH = '9394ed120afb08e4'   # the sheet's VRAM bytes (M1_TEXLOG)
CELLS = [(0, 0, 'High'), (1, 0, 'Mid'), (2, 0, 'S\u00b7Mid'), (3, 0, 'Low'), (0, 1, '!')]
DARK, GREY_LIGHT, GOLD_DARK, GOLD, RAMP0, WHITE = 1, 4, 5, 6, 7, 15

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'port', 'm1', 'textures')
FONTS = 'C:/Windows/Fonts'


def mask(draw_fn):
    """coverage (0-1) at art size of a shape drawn at SS times that size"""
    im = Image.new('L', (CELL * SS, CELL * SS), 0)
    draw_fn(ImageDraw.Draw(im), SS * K)
    return np.asarray(im.resize((CELL, CELL), Image.BOX), dtype=np.float32) / 255.0


def bolt_polys(seed):
    """the jagged diagonal burst (texels): a band tapering to a point at the lower left and the
    upper right, and large irregular spikes on both sides, bursting outwards from the middle"""
    rnd = random.Random(seed)
    a, b = np.array([3.0, 61.0]), np.array([61.0, 3.0])
    axis = b - a
    length = np.linalg.norm(axis)
    u = axis / length
    n = np.array([u[1], -u[0]])     # towards the upper left
    half = lambda s: HALF_WIDTH * math.sin(math.pi * min(max(s, 0), 1)) ** 1.1
    at = lambda s, off: tuple(a + u * s * length + n * off)
    steps = 40
    band = [at(i / steps, half(i / steps)) for i in range(steps + 1)]
    band += [at(i / steps, -half(i / steps)) for i in range(steps, -1, -1)]
    polys = [band]
    for side in (1, -1):
        for c in (0.2, 0.31, 0.42, 0.58, 0.69, 0.8):
            c += rnd.uniform(-0.03, 0.03)
            base = 0.05 + 0.02 * rnd.random()
            reach = half(c) * (1.3 + 0.35 * rnd.random()) + 1.5
            lean = (1 if c > 0.5 else -1) * (5.0 + 4.0 * rnd.random())    # swept towards the nearer tip
            tip = tuple(np.clip(a + u * (c * length + lean) + n * side * reach, 1.5, 62.5))   # inside the cell
            polys.append([at(c - base, side * half(c - base) * 0.7), tip, at(c + base, side * half(c + base) * 0.7)])
    return polys, a, u, n, length


def bolt_layers(seed):
    polys, a, u, n, length = bolt_polys(seed)

    def draw(d, sc):
        for poly in polys:
            d.polygon([(x * sc, y * sc) for x, y in poly], fill=255)
    body = mask(draw)
    # the dark rim: the shape grown by about a texel, under the body
    grown = np.asarray(Image.fromarray((body * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(5)), dtype=np.float32) / 255
    grown = np.asarray(Image.fromarray((grown * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)), dtype=np.float32) / 255
    # the ramp: mid blue in the body, lighter towards the edges and the tips, streaks along it
    ys, xs = np.mgrid[0:CELL, 0:CELL].astype(np.float32)
    px = (xs + 0.5) / K - a[0]
    py = (ys + 0.5) / K - a[1]
    s = (px * u[0] + py * u[1]) / length
    d = px * n[0] + py * n[1]
    rnd = np.random.default_rng(seed)
    across = np.zeros_like(d)
    for _ in range(13):     # thin light streaks parallel to the bolt, fading along it
        off, wid, amp = rnd.uniform(-9, 9), rnd.uniform(0.3, 0.9), rnd.uniform(0.25, 0.6)
        lo, hi = sorted(rnd.uniform(0, 1, 2))
        along = np.clip(1 - np.abs((s - (lo + hi) / 2) / max((hi - lo) / 2, 0.08)), 0, 1)
        across += amp * np.exp(-((d - off) / wid) ** 2) * along
    width = HALF_WIDTH * 1.4 * np.clip(np.sin(np.pi * np.clip(s, 0, 1)), 0.05, 1) ** 0.7
    edge = np.clip(np.abs(d) / width, 0, 1) ** 2
    tips = np.clip(1 - np.minimum(s, 1 - s) / 0.3, 0, 1) ** 1.3
    ramp = np.clip(0.12 + 0.35 * edge + 0.8 * tips + across, 0, 1)
    return body, grown, ramp


def text_layers(word, font_path, target_cap=12.0, max_w=58.0):
    """fill and outline coverage of the word, and its shading (0 top .. 1 baseline); the cap
    height as the game's (texels), smaller only if the word would leave the cell"""
    big = SS * K
    size = 60 * big
    while True:
        f = ImageFont.truetype(font_path, size)
        x0, y0, x1, y1 = f.getbbox('H' if word != '!' else '!')
        cap = (y1 - y0) / big
        w = (f.getbbox(word)[2] - f.getbbox(word)[0]) / big
        if cap <= target_cap and w <= max_w:
            break
        size -= big // 4
    bx0, by0, bx1, by1 = f.getbbox(word)
    hx0, hy0, hx1, hy1 = f.getbbox('H' if word != '!' else '!')
    ox = 32 * big - (bx0 + bx1) / 2
    oy = 31 * big - (hy0 + hy1) / 2
    stroke = int(1.1 * big)
    fill = mask(lambda d, sc: d.text((ox, oy), word, font=f, fill=255))
    outline = mask(lambda d, sc: d.text((ox, oy), word, font=f, fill=255, stroke_width=stroke, stroke_fill=255))
    ys = (np.mgrid[0:CELL, 0:CELL][0].astype(np.float32) + 0.5) / K
    top, base = (oy + hy0) / big, (oy + hy1) / big
    shade = np.clip((ys - top) / (base - top), 0, 1)
    return fill, outline, shade


def composite(layers):
    """layers bottom to top: (coverage, index a, index b, mix); returns weights over 16 entries"""
    wts = np.zeros((CELL, CELL, 16), dtype=np.float32)
    for cov, ia, ib, t in layers:
        wts *= (1 - cov)[..., None]
        wts[..., ia] += cov * (1 - t)
        wts[..., ib] += cov * t
    return wts


def encode(wts):
    """the two strongest entries, their mix and the coverage as RGBA"""
    total = wts.sum(-1)
    order = np.argsort(-wts, axis=-1)
    a, b = order[..., 0], order[..., 1]
    wa = np.take_along_axis(wts, a[..., None], -1)[..., 0]
    wb = np.take_along_axis(wts, b[..., None], -1)[..., 0]
    t = np.where(wa + wb > 0, wb / np.maximum(wa + wb, 1e-6), 0)
    out = np.zeros((CELL, CELL, 4), dtype=np.uint8)
    out[..., 0], out[..., 1] = a, b
    out[..., 2] = np.round(t * 255)
    out[..., 3] = np.round(np.clip(total, 0, 1) * 255)
    return out


def ramp_pair(r):
    """a 0-1 ramp position as two neighbouring ramp entries and the mix"""
    pos = RAMP0 + r * (WHITE - RAMP0)
    ia = np.clip(np.floor(pos), RAMP0, WHITE - 1).astype(np.int64)
    return ia, ia + 1, pos - ia


def cell(word, seed, fonts):
    body, rim, ramp = bolt_layers(seed)
    ra, rb, rt = ramp_pair(ramp)
    layers = [(rim * 0.9, DARK, DARK, np.zeros_like(rim))]
    wts = composite(layers)
    # the body: the ramp's two entries per pixel
    wts *= (1 - body)[..., None]
    np.put_along_axis(wts, ra[..., None], np.take_along_axis(wts, ra[..., None], -1) + (body * (1 - rt))[..., None], -1)
    np.put_along_axis(wts, rb[..., None], np.take_along_axis(wts, rb[..., None], -1) + (body * rt)[..., None], -1)
    if word == '!':
        fill, outline, shade = text_layers('!', fonts['italic'], target_cap=21.0)
        top = [(outline, DARK, DARK, np.zeros_like(shade)), (fill, GOLD, GOLD_DARK, np.clip(shade - 0.45, 0, 1) * 1.6)]
    else:
        fill, outline, shade = text_layers(word, fonts['bold'])
        top = [(outline, DARK, DARK, np.zeros_like(shade)), (fill, WHITE, GREY_LIGHT, np.clip(shade - 0.5, 0, 1) * 1.7)]
    for cov, ia, ib, t in top:
        t = np.clip(t, 0, 1)
        wts *= (1 - cov)[..., None]
        wts[..., ia] += cov * (1 - t)
        wts[..., ib] += cov * t
    return encode(wts)


def preview(sheet, path):
    """the art in the light blue, deep blue and a yellow palette, on grey"""
    base = [(0, 0, 0), (41, 49, 49), (90, 90, 99), (123, 123, 132), (148, 156, 156), (173, 132, 0), (247, 189, 0)]
    ramps = [[(33, 107, 198), (33, 132, 255), (57, 148, 255), (90, 165, 255), (123, 181, 255), (156, 198, 255), (189, 214, 255), (222, 231, 255)],
             [(16, 33, 198), (33, 49, 239), (57, 74, 239), (90, 107, 239), (123, 132, 239), (156, 165, 247), (189, 189, 247), (222, 222, 247)],
             [(198, 132, 0), (239, 165, 0), (247, 181, 33), (247, 198, 66), (255, 214, 99), (255, 222, 140), (255, 231, 181), (255, 239, 222)]]
    rows = []
    for ramp in ramps:
        pal = np.array(base + ramp + [(255, 255, 255)], dtype=np.float32)
        a, b, t, al = sheet[..., 0], sheet[..., 1], sheet[..., 2] / 255.0, sheet[..., 3:4] / 255.0
        rgb = pal[a] * (1 - t[..., None]) + pal[b] * t[..., None]
        rows.append(rgb * al + 128 * (1 - al))
    Image.fromarray(np.concatenate(rows, 0).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--font', default=os.path.join(FONTS, 'BOOKOSB.TTF'))
    ap.add_argument('--italic', default=os.path.join(FONTS, 'BOOKOSBI.TTF'))
    ap.add_argument('--preview', help='also write the art in three palettes to this file')
    args = ap.parse_args()
    fonts = {'bold': args.font, 'italic': args.italic}
    sheet = np.zeros((2 * CELL, 4 * CELL, 4), dtype=np.uint8)
    for i, (cx, cy, word) in enumerate(CELLS):
        sheet[cy * CELL:(cy + 1) * CELL, cx * CELL:(cx + 1) * CELL] = cell(word, 11 + i, fonts)
    os.makedirs(OUT, exist_ok=True)
    Image.fromarray(sheet, 'RGBA').save(os.path.join(OUT, 'hitlabels.png'))
    with open(os.path.join(OUT, 'replace.txt'), 'w', newline='\n') as f:
        f.write('# replacement art (port/m1/tex.c): hash x y w h file; made by tools/hitlabels.py\n')
        f.write('%s 0 128 256 128 hitlabels.png\n' % HASH)
    if args.preview:
        preview(sheet, args.preview)
    print('wrote', os.path.join(OUT, 'hitlabels.png'))


if __name__ == '__main__':
    main()
