#!/usr/bin/env python3
# usage: python3 -I tools/hd/clock_face.py assets/hd/cockpit.png assets/hd/clock.png
"""Makes the HD clock face of automatic cars (clock.png): the original's clock (LIB2 #240) sits where the
tachometer is, so the face is drawn over the HD tachometer's face (centre (581, 569) of cockpit.png, radius
90) with the hour marks and the numerals 1..12; outside the face the picture is clear and the cockpit's bezel
shows. The hands are drawn at run time (enh_hd.c)."""
import math
import sys
from PIL import Image, ImageDraw, ImageFont

src, out = sys.argv[1:3]
CX, CY, R, SIZE = 581, 569, 90, 260               # SIZE x SIZE around the centre
cockpit = Image.open(src).convert('RGBA')
face = Image.new('RGBA', (SIZE * 4, SIZE * 4), (0, 0, 0, 0))   # drawn at 4x, then reduced (smooth edges)
d = ImageDraw.Draw(face)
c = SIZE * 2
r = R * 4
d.ellipse((c - r, c - r, c + r, c + r), fill=(12, 12, 14, 255))
font = ImageFont.load_default(size=23 * 4)
for h in range(12):
    a = math.radians(90 - 30 * h)                  # 12 at the top
    long_mark = h % 3 == 0
    r0, r1 = r - (14 if long_mark else 9) * 4, r - 3 * 4
    w = (5 if long_mark else 3) * 4
    d.line((c + r0 * math.cos(a), c - r0 * math.sin(a), c + r1 * math.cos(a), c - r1 * math.sin(a)),
           fill=(240, 240, 240, 255), width=w)
    for m in range(1, 5):                          # minute dots
        am = math.radians(90 - 30 * h - 6 * m)
        rd = r - 6 * 4
        x, y = c + rd * math.cos(am), c - rd * math.sin(am)
        d.ellipse((x - 5, y - 5, x + 5, y + 5), fill=(200, 200, 200, 255))
    num = str(h if h else 12)
    rn = r - 29 * 4
    x, y = c + rn * math.cos(a), c - rn * math.sin(a)
    d.text((x, y), num, font=font, fill=(245, 245, 245, 255), anchor='mm')
face = face.resize((SIZE, SIZE), Image.LANCZOS)
face.save(out)
preview = cockpit.crop((CX - SIZE // 2, CY - SIZE // 2, CX + SIZE // 2, CY + SIZE // 2))
preview.alpha_composite(face)
preview.save(out.replace('.png', '_preview.png') if len(sys.argv) < 4 else sys.argv[3])
