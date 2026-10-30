"""SR_ENH_DUMP .idx files -> PNG (RGBA, index FFh transparent), plus a 4x nearest copy and the mask.

usage: python3 -I tools/hd/idx2png.py DUMPDIR OUTDIR
Palette: palette_race.txt (or palette_town.txt for *_town files) in DUMPDIR.
"""
import os
import sys

import numpy as np
from PIL import Image


def load_palette(path):
    with open(path) as f:
        return [int(line, 16) for line in f if line.strip()]


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    pals = {k: load_palette(os.path.join(src, f"palette_{k}.txt"))
            for k in ("race", "town") if os.path.exists(os.path.join(src, f"palette_{k}.txt"))}
    for name in sorted(os.listdir(src)):
        if not name.endswith(".idx"):
            continue
        with open(os.path.join(src, name), "rb") as f:
            head = f.readline().split()
            w, h = int(head[1]), int(head[2])
            idx = np.frombuffer(f.read(w * h), dtype=np.uint8).reshape(h, w)
        pal = pals["town" if "town" in name else "race"]
        lut = np.zeros((256, 4), dtype=np.uint8)
        for i, c in enumerate(pal):
            lut[i] = (c >> 16 & 255, c >> 8 & 255, c & 255, 255)
        rgba = lut[idx]
        base = name[:-4]
        Image.fromarray(rgba, "RGBA").save(os.path.join(out, f"{base}.png"))
        big = Image.fromarray(rgba, "RGBA").resize((w * 4, h * 4), Image.NEAREST)
        big.save(os.path.join(out, f"{base}_x4.png"))
        mask = ((idx != 0xFF) * 255).astype(np.uint8)
        Image.fromarray(mask, "L").save(os.path.join(out, f"{base}_mask.png"))
        print(f"{base}: {w}x{h}, transparent {int((idx == 0xFF).sum())} px")


if __name__ == "__main__":
    main()
