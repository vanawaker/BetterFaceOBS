#!/usr/bin/env python3
"""Landmark jitter report from a BETTERFACE_DUMP csv.

jitter.py DUMP [--still]
--still: the clip is a frozen frame (plus noise), so any motion is jitter: report the spread
around each point's median. Otherwise report the high-frequency part (point minus a centered
9-frame moving average), which is what shows up as shaking.
Numbers are in percent of the face width (contour point 0 to 16).
"""
import argparse
import csv
from collections import defaultdict

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("dump")
ap.add_argument("--still", action="store_true")
ap.add_argument("--skip", type=int, default=15, help="frames to ignore after a face appears")
a = ap.parse_args()

rows = defaultdict(list)
for r in csv.reader(open(a.dump)):
    if len(r) < 11 + 87 * 4:
        continue
    fid, tid, alive, pres, new = int(r[0]), int(r[2]), int(r[3]), float(r[4]), int(r[5])
    box = list(map(float, r[6:10]))
    anchors = int(r[10])
    pts = np.array(list(map(float, r[11:11 + 87 * 4]))).reshape(87, 4)
    rows[tid].append((fid, alive, pres, new, box, pts, anchors))

GROUPS = {"contour": range(0, 17), "chin": range(6, 11), "eyes": range(17, 29), "nose": range(41, 49),
          "lips": range(65, 79)}

def spread(x, still):
    # x: frames x points x 2
    if still:
        d = x - np.median(x, axis=0, keepdims=True)
    else:
        k = 9
        ker = np.ones(k) / k
        sm = np.apply_along_axis(lambda v: np.convolve(v, ker, mode="same"), 0, x)
        d = (x - sm)[k:-k]
    mag = np.linalg.norm(d, axis=2)
    step = np.linalg.norm(np.diff(x, axis=0), axis=2)
    return np.sqrt((mag ** 2).mean()), np.percentile(mag, 99), np.percentile(step, 99)

for tid, rs in sorted(rows.items()):
    rs = [r for r in rs if r[1]]
    if len(rs) < a.skip + 20:
        print(f"face {tid}: only {len(rs)} frames")
        continue
    rs = rs[a.skip:]
    raw = np.array([r[5][:, 0:2] for r in rs if r[3]])
    smo = np.array([r[5][:, 2:4] for r in rs if r[3]])
    fw = np.median(np.linalg.norm(smo[:, 16] - smo[:, 0], axis=1))
    boxes = np.array([r[4] for r in rs])
    box_moves = int((np.abs(np.diff(boxes, axis=0)).max(axis=1) > 1e-6).sum())
    lost = sum(1 for r in rs if not r[3])
    print(f"face {tid}: {len(rs)} frames, face width {fw:.0f}px, box moves {box_moves}, "
          f"re-anchors {rs[-1][6] - rs[0][6]}, frames without landmarks {lost}")
    for g, idx in GROUPS.items():
        idx = list(idx)
        r_rms, r_p99, r_step = spread(raw[:, idx], a.still)
        s_rms, s_p99, s_step = spread(smo[:, idx], a.still)
        f = 100.0 / fw
        gap = np.sqrt((np.linalg.norm(smo[:, idx] - raw[:, idx], axis=2) ** 2).mean())
        print(f"  {g:8s} raw rms {r_rms*f:5.2f}% p99 {r_p99*f:5.2f}% step99 {r_step*f:5.2f}%   "
              f"smooth rms {s_rms*f:5.2f}% p99 {s_p99*f:5.2f}% step99 {s_step*f:5.2f}%  ({s_rms:.1f}px)  "
              f"smooth-raw {gap*f:4.2f}%")
