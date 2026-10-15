#!/usr/bin/env python3
"""Side-by-side crops of screenshots: compare.py OUT x0 y0 x1 y1 IMG [IMG ...] [--scale S]"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("box", nargs=4, type=int)
    ap.add_argument("images", nargs="+")
    ap.add_argument("--scale", type=float, default=1.0)
    a = ap.parse_args()
    crops = []
    for p in a.images:
        im = Image.open(p).convert("RGB").crop(tuple(a.box))
        if a.scale != 1.0:
            im = im.resize((int(im.width * a.scale), int(im.height * a.scale)), Image.LANCZOS)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, 8 + 7 * len(Path(p).stem), 16], fill=(0, 0, 0))
        d.text((4, 2), Path(p).stem, fill=(255, 255, 0))
        crops.append(im)
    w = sum(c.width for c in crops) + 4 * (len(crops) - 1)
    out = Image.new("RGB", (w, crops[0].height), "white")
    x = 0
    for c in crops:
        out.paste(c, (x, 0))
        x += c.width + 4
    out.save(a.out, quality=90)
    print(a.out, out.size)


if __name__ == "__main__":
    main()
