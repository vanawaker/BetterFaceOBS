#!/usr/bin/env python3
"""Capture the playing test clip as a frame sequence, filter off then on, and build an A/B video.

capture_seq.py TAG [--width W] [key=value ...]
Output: testmedia/seq/TAG_ab.mp4 (left: off, right: on)
"""
import argparse
import base64
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obsctl  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("tag")
ap.add_argument("pairs", nargs="*")
ap.add_argument("--width", type=int, default=960)
ap.add_argument("--seconds", type=float, default=0)
ap.add_argument("--label", default="")
a = ap.parse_args()

root = Path(__file__).resolve().parent.parent / "testmedia" / "seq"
cl = obsctl.client()
def _val(v):
    x = obsctl.parse_value(v)
    return int(x) if isinstance(x, float) and x.is_integer() else x


upd = {k: _val(v) for k, v in (p.split("=", 1) for p in a.pairs)}
if upd:
    cl.set_source_filter_settings(obsctl.INPUT, obsctl.FILTER, upd, True)
sw, sh = obsctl.source_size(cl)
w = a.width
h = int(round(sh * w / sw / 2)) * 2
dur = a.seconds or float(cl.get_media_input_status(obsctl.INPUT).media_duration) / 1000.0 - 0.3

lists = {}
start_times = {}
for mode in ("off", "on"):
    d = root / f"{a.tag}_{mode}"
    d.mkdir(parents=True, exist_ok=True)
    for f in d.glob("*.jpg"):
        f.unlink()
    cl.set_source_filter_enabled(obsctl.INPUT, obsctl.FILTER, mode == "on")
    cl.trigger_media_input_action(obsctl.INPUT, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_RESTART")
    time.sleep(0.3)
    t0 = time.time()
    frames = []
    i = 0
    while time.time() - t0 < dur:
        r = cl.get_source_screenshot(obsctl.INPUT, "jpg", w, h, 92)
        cursor = cl.get_media_input_status(obsctl.INPUT).media_cursor or 0
        (d / f"{i:05d}.jpg").write_bytes(base64.b64decode(r.image_data.split(",", 1)[1]))
        frames.append((cursor / 1000.0, i))
        i += 1
    # Order by media time so both runs share the clip's own timeline
    if len({t for t, _ in frames}) < len(frames) // 4:
        raise SystemExit(f"{mode}: media cursor did not advance, capture is unusable")
    frames.sort()
    lines = []
    for k, (t, idx) in enumerate(frames):
        nxt = frames[k + 1][0] if k + 1 < len(frames) else t + 0.04
        if nxt - t <= 0:
            continue
        lines.append(f"file '{d}/{idx:05d}.jpg'")
        lines.append(f"duration {nxt - t:.4f}")
    start_times[mode] = frames[0][0]
    lst = d / "list.txt"
    lst.write_text("\n".join(lines) + "\n")
    lists[mode] = lst
    print(mode, i, "frames", f"{i / dur:.1f} fps")
cl.set_source_filter_enabled(obsctl.INPUT, obsctl.FILTER, True)

def label_png(text: str, path: Path) -> Path:
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype("/System/Library/Fonts/Hiragino Sans GB.ttc", max(18, w // 30))
    box = font.getbbox(text)
    im = Image.new("RGBA", (box[2] + 24, box[3] + 16), (0, 0, 0, 150))
    ImageDraw.Draw(im).text((12, 6), text, font=font, fill=(255, 255, 255, 255))
    im.save(path)
    return path


out = root / f"{a.tag}_ab.mp4"
t_common = max(start_times.values())
l_off = label_png("原图", root / f"{a.tag}_label_off.png")
l_on = label_png(a.label or "BetterFace", root / f"{a.tag}_label_on.png")
subprocess.run([
    "ffmpeg", "-v", "error", "-y",
    "-f", "concat", "-safe", "0", "-i", str(lists["off"]),
    "-f", "concat", "-safe", "0", "-i", str(lists["on"]),
    "-i", str(l_off), "-i", str(l_on),
    "-filter_complex",
    f"[0:v]fps=25,trim=start={t_common - start_times['off']:.3f},setpts=PTS-STARTPTS[a0];"
    f"[1:v]fps=25,trim=start={t_common - start_times['on']:.3f},setpts=PTS-STARTPTS[b0];"
    "[a0][2:v]overlay=12:12[a];[b0][3:v]overlay=12:12[b];[a][b]hstack=inputs=2:shortest=1[v]",
    "-map", "[v]", "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
    str(out)], check=True)
print("wrote", out)
