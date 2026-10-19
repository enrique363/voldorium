"""Draws the Voldorium icon (a stylized katar on a crimson badge) and writes m1/voldorium.ico.

The katar: a broad steel blade pointing up with a bright centre ridge, a gold crossguard with a
red gem, two gold side bars and two hand bars (the H-shaped grip). Drawn with 4x4 supersampling
at 256 px; the smaller icon sizes are box-filtered from it. Also writes art/voldorium_256.png as
a preview. Usage: python -I art/make_icon.py
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
N = 256
SS = 4


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))


def clamp01(t):
    return 0.0 if t < 0 else 1.0 if t > 1 else t


def rrect(x, y, x0, y0, x1, y1, r):
    """inside a rounded rectangle"""
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r and x0 <= x <= x1 and y0 <= y <= y1


TIP, BASE, HALF = 6.0, 150.0, 34.0
ANGLE = math.radians(40)     # the katar leans: the blade points to the upper right
SCALE = 0.86                 # and shrinks a little to stay inside the badge when tilted


def blade_half(y, grow=0.0):
    """half width of the leaf-shaped blade: widest a little above the guard"""
    if y < TIP - grow * 1.6 or y > BASE + grow:
        return -1
    t = clamp01((y - (TIP - grow * 1.6)) / (BASE - TIP + grow * 1.6))
    w = min(1.0, 1.12 * t ** 0.72) * (1 - 0.16 * clamp01((t - 0.82) / 0.18))
    return HALF * w + grow


def bar_x(y, outer, bottom_out):
    """side bars flare outward toward the bottom: x of a bar edge at y"""
    t = clamp01((y - 160) / 80.0)
    return outer + bottom_out * t


def gold(y, y0, y1):
    t = clamp01((y - y0) / max(1.0, y1 - y0))
    return lerp((250, 214, 120), (164, 112, 34), t)


def sample(x, y):
    """colour (r, g, b, a) at a point of the 256 x 256 icon"""
    col = (0.0, 0.0, 0.0, 0.0)
    bx, by = x, y    # badge coordinates; the katar is drawn in its own, tilted frame
    dx, dy = (x - 128) / SCALE, (y - 128) / SCALE
    x = 128 + dx * math.cos(ANGLE) + dy * math.sin(ANGLE)
    y = 128 - dx * math.sin(ANGLE) + dy * math.cos(ANGLE)

    def over(c, a=1.0):
        nonlocal col
        a = a * (c[3] if len(c) > 3 else 1.0)
        r, g, b = c[0], c[1], c[2]
        ca = col[3]
        out_a = a + ca * (1 - a)
        if out_a <= 0:
            return
        col = ((r * a + col[0] * ca * (1 - a)) / out_a, (g * a + col[1] * ca * (1 - a)) / out_a,
               (b * a + col[2] * ca * (1 - a)) / out_a, out_a)

    # badge: crimson to near black, gold rim
    if rrect(bx, by, 6, 6, 250, 250, 46):
        over((196, 150, 70))
        if rrect(bx, by, 10, 10, 246, 246, 42):
            d = math.hypot(bx - 120, by - 110) / 175.0
            over(lerp((128, 18, 32), (20, 4, 8), clamp01(d)))
            # warm glow behind the blade
            g = math.exp(-((x - 128) ** 2 + (y - 84) ** 2) / (2 * 50.0 ** 2))
            over((255, 196, 120), 0.30 * g)
    # blade: dark outline, steel halves, ridge
    hw = blade_half(y, 3.0)
    if hw >= 0 and abs(x - 128) <= hw:
        over((26, 24, 32))
    hw = blade_half(y)
    if hw >= 0 and abs(x - 128) <= hw:
        t = clamp01((y - TIP) / (BASE - TIP))
        if x < 128:
            over(lerp((236, 240, 247), (176, 184, 198), t))
        else:
            over(lerp((150, 158, 172), (96, 102, 116), t))
        # fuller (shallow groove) either side of the ridge
        if 6 < abs(x - 128) < 9 and TIP + 30 < y < BASE - 6:
            over((60, 64, 78), 0.35)
        if abs(x - 128) < 1.6 and TIP + 8 < y < BASE - 4:
            over((255, 255, 255), 0.85)
    # side bars (behind the guard): flaring outward, pointed ends
    for side in (-1, 1):
        for grow, colour in ((2.5, None), (0.0, 1)):
            if not 152 - grow <= y <= 246 + grow:
                continue
            inner = 128 + side * (40 - grow) + side * 8 * clamp01((y - 160) / 80.0)
            outer = 128 + side * (58 + grow) + side * 12 * clamp01((y - 160) / 80.0)
            lo, hi = min(inner, outer), max(inner, outer)
            if y > 226:    # the point: both edges meet
                k = clamp01((y - 226) / (20 + grow))
                mid = (lo + hi) / 2 + side * 4 * k
                lo, hi = lo + (mid - lo) * k, hi - (hi - mid) * k
            if lo <= x <= hi:
                if colour is None:
                    over((52, 30, 8))
                else:
                    c = gold(y, 154, 244)
                    over(c if (x - lo) / max(1, hi - lo) < 0.55 else lerp(c, (90, 60, 20), 0.3))
    # hand bars between the side bars
    for hy0, hy1 in ((186, 196), (206, 216)):
        if 86 <= x <= 170 and hy0 - 2 <= y <= hy1 + 2:
            over((52, 30, 8))
        if 86 <= x <= 170 and hy0 <= y <= hy1:
            over(lerp((214, 168, 72), (130, 84, 24), clamp01((y - hy0) / (hy1 - hy0))))
    # crossguard (curved: its ends dip) and gem
    cy = y - 0.0028 * (x - 128) ** 2
    if rrect(x, cy, 62, 140, 194, 168, 8):
        over((52, 30, 8))
    if rrect(x, cy, 65, 143, 191, 165, 6):
        over(gold(cy, 143, 165))
        if 145 <= cy <= 148:
            over((255, 240, 190), 0.6)
    gd = math.hypot(x - 128, y - 155)
    if gd <= 10:
        over((60, 10, 14))
    if gd <= 8:
        over(lerp((236, 40, 64), (120, 6, 20), clamp01(gd / 8)))
        if math.hypot(x - 125, y - 152) <= 2.6:
            over((255, 220, 225), 0.9)
    return col


def render():
    px = []
    for py in range(N):
        row = []
        for qx in range(N):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    r, g, b, a = sample(qx + (sx + 0.5) / SS, py + (sy + 0.5) / SS)
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            n = SS * SS
            a = acc[3] / n
            if a > 0:
                row.append((acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], a))
            else:
                row.append((0, 0, 0, 0))
        px.append(row)
    return px


def downscale(px, size):
    f = N // size
    out = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for yy in range(y * f, y * f + f):
                for xx in range(x * f, x * f + f):
                    r, g, b, a = px[yy][xx]
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            a = acc[3] / (f * f)
            row.append((acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], a) if acc[3] > 0 else (0, 0, 0, 0))
        out.append(row)
    return out


def png(px):
    h, w = len(px), len(px[0])
    raw = b''.join(b'\0' + bytes(
        v for (r, g, b, a) in row for v in (int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a * 255 + 0.5))) for row in px)

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def downscale_any(px, size):
    """box filter for sizes that do not divide 256 (48, 24): through the next multiple"""
    if N % size == 0:
        return downscale(px, size)
    big = size * (N // size)
    # resample 256 -> big by nearest, then box
    tmp = [[px[int(y * N / big)][int(x * N / big)] for x in range(big)] for y in range(big)]
    f = big // size
    out = []
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for yy in range(y * f, y * f + f):
                for xx in range(x * f, x * f + f):
                    r, g, b, a = tmp[yy][xx]
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            a = acc[3] / (f * f)
            row.append((acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], a) if acc[3] > 0 else (0, 0, 0, 0))
        out.append(row)
    return out


def main():
    px = render()
    open(os.path.join(HERE, 'voldorium_256.png'), 'wb').write(png(px))
    sizes = [256, 128, 64, 48, 32, 24, 16]
    images = [png(px if s == N else downscale_any(px, s)) for s in sizes]
    head = struct.pack('<HHH', 0, 1, len(sizes))
    off = 6 + 16 * len(sizes)
    dirs = b''
    for s, data in zip(sizes, images):
        dirs += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    out = os.path.join(ROOT, 'm1', 'voldorium.ico')
    open(out, 'wb').write(head + dirs + b''.join(images))
    print('wrote', out, 'and art/voldorium_256.png')


if __name__ == '__main__':
    main()
