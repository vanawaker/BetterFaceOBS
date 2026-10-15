#!/usr/bin/env python3
"""Where does the filter add frame-to-frame change? tdiff.py OFF.mp4 ON.mp4 [ON2.mp4 ...] --out PNG

For each recording: mean |luma(t) - luma(t-1)| per pixel over the whole clip (960 px wide).
The PNG shows, per ON recording, the excess over OFF in red (added shaking) and blue (calmer).
"""
import argparse
import subprocess

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("videos", nargs="+")
ap.add_argument("--out", required=True)
ap.add_argument("--width", type=int, default=960)
a = ap.parse_args()

def tmap(path):
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=width,height",
                            "-of", "csv=p=0", path], capture_output=True, text=True).stdout.strip().split(",")
    w = a.width
    h = int(round(int(probe[1]) * w / int(probe[0]) / 2)) * 2
    p = subprocess.Popen(["ffmpeg", "-v", "error", "-i", path, "-vf", f"scale={w}:{h}", "-f", "rawvideo", "-pix_fmt",
                          "gray", "-"], stdout=subprocess.PIPE)
    acc = np.zeros((h, w)); prev = None; n = 0; first = None
    while True:
        buf = p.stdout.read(w * h)
        if len(buf) < w * h:
            break
        f = np.frombuffer(buf, np.uint8).reshape(h, w).astype(np.float32)
        if prev is not None:
            acc += np.abs(f - prev); n += 1
        else:
            first = f
        prev = f
    return acc / max(n, 1), first, n

off, base, n0 = tmap(a.videos[0])
panels = []
for v in a.videos[1:]:
    m, _, n = tmap(v)
    d = m - off
    print(f"{v}: frames {n} (off {n0}), mean diff on {m.mean():.2f} off {off.mean():.2f}, "
          f"pixels with +4 or more: {(d > 4).mean()*100:.2f}%, +8 or more: {(d > 8).mean()*100:.2f}%")
    g = np.stack([base] * 3, -1) * 0.5
    r = np.clip(d / 8.0, 0, 1); b = np.clip(-d / 8.0, 0, 1)
    g[..., 0] += r * 255 * 0.8; g[..., 2] += b * 255 * 0.8
    panels.append(np.clip(g, 0, 255).astype(np.uint8))
Image.fromarray(np.concatenate(panels, 0)).save(a.out)
