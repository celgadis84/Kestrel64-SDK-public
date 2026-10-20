"""Procedural art for demo 02 (regenerate: python make_assets.py). Public domain."""
import math
from PIL import Image
# shaded ball 32x32 with alpha
b = Image.new('RGBA', (32, 32), (0, 0, 0, 0))
for y in range(32):
    for x in range(32):
        dx, dy = (x - 15.5) / 15.5, (y - 15.5) / 15.5
        r2 = dx * dx + dy * dy
        if r2 <= 1.0:
            nz = math.sqrt(1 - r2)
            l = max(0.0, -0.4 * dx - 0.5 * dy + 0.77 * nz)
            spec = max(0.0, l) ** 24
            c = 0.25 + 0.75 * l
            b.putpixel((x, y), (min(255, int(255 * c + 255 * spec)), min(255, int(140 * c + 255 * spec)),
                                min(255, int(40 * c + 255 * spec)), 255))
b.save('ball.png')
# 32x32 background tile: blue bricks
t = Image.new('RGBA', (32, 32))
for y in range(32):
    for x in range(32):
        row = y // 8
        off = 8 if row & 1 else 0
        mortar = (y % 8 == 0) or ((x + off) % 16 == 0)
        n = ((x * 7 + y * 13) ^ (x * y)) & 15
        c = (30, 34, 60) if mortar else (40 + n, 60 + n, 120 + 2 * n)
        t.putpixel((x, y), c + (255,))
t.save('bricks.png')
