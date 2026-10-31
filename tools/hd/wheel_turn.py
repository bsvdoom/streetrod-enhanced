#!/usr/bin/env python3
# usage: python3 -I tools/hd/wheel_turn.py assets/hd/cockpit.png OUTDIR DEGREES   (the release uses 11)
"""Makes the HD steering wheel turned left and right (wheel_left.png, wheel_right.png): the strip
x 320..1056, y 652..760 of cockpit.png (the original's wheel pictures, 184 x 26 at (80, 163), x 4) with the
spokes, the hub and the lower arc turned about the hub, as the original's three wheel pictures."""
import sys, math
from collections import deque
from PIL import Image

src, outdir, deg = sys.argv[1], sys.argv[2], float(sys.argv[3])
im = Image.open(src).convert('RGBA')
X0, Y0, X1, Y1 = 320, 652, 1056, 760
W, H = X1 - X0, Y1 - Y0
lum = im.convert('L').load()
px = im.load()

reds = [(x, y) for y in range(Y0, Y1) for x in range(X0, X1)
        if px[x, y][0] > 150 and px[x, y][1] < 80 and px[x, y][2] < 80]
hx = sum(p[0] for p in reds) / len(reds)
hy = sum(p[1] for p in reds) / len(reds)

def flood(seeds, ok):
    seen = set(s for s in seeds if ok(*s))
    q = deque(seen)
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if X0 <= nx < X1 and Y0 <= ny < Y1 and (nx, ny) not in seen and ok(nx, ny):
                seen.add((nx, ny))
                q.append((nx, ny))
    return seen

panel = flood([(470, 664), (900, 664), (420, 670), (960, 670)], lambda x, y: lum[x, y] >= 120)
SIDE, HUB_R = 34, 66
def ok(x, y):
    if lum[x, y] < 30 or (x, y) in panel or not (X0 + SIDE <= x < X1 - SIDE):
        return False
    return y >= hy - 20 or math.hypot(x - hx, y - hy) <= HUB_R      # above the hub only the hub itself
seeds = [(int(hx), int(hy)), (int(hx) - 60, int(hy)), (int(hx) + 60, int(hy)),
         (int(hx), Y1 - 8), (int(hx) - 200, Y1 - 25), (int(hx) + 200, Y1 - 25)]
part = flood(seeds, ok)
outline = set()
for (x, y) in part:
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            p = (x + dx, y + dy)
            if X0 <= p[0] < X1 and Y0 <= p[1] < Y1 and p not in part and p not in panel and lum[p] < 30 \
                    and (p[1] >= hy - 20 or math.hypot(p[0] - hx, p[1] - hy) <= HUB_R + 3):
                outline.add(p)
sel = part | outline

strip = im.crop((X0, Y0, X1, Y1))
sp = strip.load()
# the turning layer
layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
lp = layer.load()
for (x, y) in sel:
    lp[x - X0, y - Y0] = sp[x - X0, y - Y0]
# the background under it: each column's gap filled between the colours above and below it
bg = strip.copy()
bp = bg.load()
inside = [[False] * H for _ in range(W)]
for (x, y) in sel:
    inside[x - X0][y - Y0] = True
for x in range(W):
    y = 0
    while y < H:
        if not inside[x][y]:
            y += 1
            continue
        y0 = y
        while y < H and inside[x][y]:
            y += 1
        top = bp[x, y0 - 1] if y0 > 0 else None
        bot = bp[x, y] if y < H else None
        dark = bot if bot and sum(bot[:3]) < 120 else (14, 14, 16, 255)
        for k in range(y0, y):
            if top and k - y0 < 3:                       # the edge above, then the dark inside
                t = (k - y0 + 1) / 4
                bp[x, k] = tuple(int(top[i] * (1 - t) + dark[i] * t) for i in range(4))
            else:
                bp[x, k] = dark
cx, cy = hx - X0, hy - Y0
for name, a in (('wheel_left', deg), ('wheel_right', -deg)):     # left: counterclockwise on the screen
    rot = layer.rotate(a, resample=Image.BICUBIC, center=(cx, cy))
    out = bg.copy()
    out.alpha_composite(rot)
    out.save(f'{outdir}/{name}.png')
view = Image.new('RGBA', (W, H * 3), (0, 0, 0, 255))
view.paste(Image.open(f'{outdir}/wheel_left.png'), (0, 0))
view.paste(strip, (0, H))
view.paste(Image.open(f'{outdir}/wheel_right.png'), (0, 2 * H))
view.save(f'{outdir}/wheel_view.png')
print('hub', round(hx, 1), round(hy, 1), 'selected', len(sel))
