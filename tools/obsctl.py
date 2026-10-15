#!/usr/bin/env python3
"""Drive a local OBS over obs-websocket to test the BetterFace plugin.

  obsctl.py setup VIDEO            scene + media source + BetterFace filter
  obsctl.py load                   sync plugin/data into the built plugin and re-create the filter
                                   (the effect file is re-read on filter creation)
  obsctl.py restart [--env K=V]    quit and relaunch OBS (needed after rebuilding the binary)
  obsctl.py set key=value ...      change filter settings (smooth=60 slim=30 debug=true ...)
  obsctl.py shot NAME [--at SEC]   pause at SEC, save shots/NAME_on.png and NAME_off.png
  obsctl.py stats                  render timing from OBS
  obsctl.py log                    shader errors from the current OBS log
"""
import argparse
import base64
import subprocess
import sys
import time
from pathlib import Path

import logging
import os

import obsws_python as obs

logging.getLogger("obsws_python").setLevel(logging.CRITICAL)

ROOT = Path(__file__).resolve().parent.parent
SHOTS = ROOT / "shots"
FILTER_KIND = "betterface_filter"
SCENE = "BF Test"
INPUT = "testvid"
FILTER = "BetterFace"
LOG_DIR = Path.home() / "Library/Application Support/obs-studio/logs"


def client() -> obs.ReqClient:
    pw = os.environ.get("OBS_WEBSOCKET_PASSWORD")
    if pw is None:
        pw = (ROOT / ".obsws_password").read_text().strip()
    return obs.ReqClient(host="localhost", port=4455, password=pw, timeout=10)


def sync_data():
    installed = Path.home() / "Library/Application Support/obs-studio/plugins/BetterFace.plugin"
    if not (installed / "Contents/Info.plist").is_file():
        raise RuntimeError("Install BetterFace before syncing its shader resources")
    subprocess.run(["rsync", "-a", "--delete", str(ROOT / "plugin/data") + "/",
                    str(installed / "Contents/Resources") + "/"], check=True)


def cmd_setup(args):
    sync_data()
    cl = client()
    cl.set_video_settings(numerator=25, denominator=1, base_width=1920, base_height=1080,
                          out_width=1920, out_height=1080)
    scenes = [s["sceneName"] for s in cl.get_scene_list().scenes]
    if SCENE not in scenes:
        cl.create_scene(SCENE)
    cl.set_current_program_scene(SCENE)
    video = str(Path(args.video).resolve())
    inputs = [i["inputName"] for i in cl.get_input_list().inputs]
    settings = {"local_file": video, "looping": True, "is_local_file": True,
                "hw_decode": False, "restart_on_activate": False}
    if INPUT in inputs:
        # Swapping the file in place can crash OBS when the resolution changes
        cl.remove_input(INPUT)
        time.sleep(1.0)
    cl.create_input(SCENE, INPUT, "ffmpeg_source", settings, True)
    time.sleep(1.0)
    item = cl.get_scene_item_id(SCENE, INPUT).scene_item_id
    cl.set_scene_item_transform(SCENE, item, {"boundsType": "OBS_BOUNDS_SCALE_INNER",
                                              "boundsWidth": 1920, "boundsHeight": 1080,
                                              "positionX": 0, "positionY": 0})
    recreate_filter(cl)
    print("setup done:", video)


def recreate_filter(cl, extra: dict | None = None):
    settings = {}
    try:
        settings = cl.get_source_filter(INPUT, FILTER).filter_settings
        cl.remove_source_filter(INPUT, FILTER)
    except Exception:
        pass
    settings.update(extra or {})
    cl.create_source_filter(INPUT, FILTER, FILTER_KIND, settings)


def cmd_load(args):
    sync_data()
    mark = len(latest_log().read_text(errors="replace").splitlines())
    cl = client()
    recreate_filter(cl)
    time.sleep(0.8)
    lines = latest_log().read_text(errors="replace").splitlines()[mark:]
    errs = [ln for ln in lines if "BetterFace" in ln or "error" in ln.lower()]
    print("filter re-created")
    if errs:
        print("\n".join(errs[-40:]))


def wait_ws(timeout: float = 120.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            client().get_version()
            return True
        except Exception:
            time.sleep(1.0)
    return False


def cmd_restart(args):
    subprocess.run(["osascript", "-e", 'tell application "OBS" to quit'], check=False)
    for _ in range(60):
        if subprocess.run(["pgrep", "-x", "OBS"], capture_output=True).returncode != 0:
            break
        time.sleep(0.5)
    env = ["--env", "BETTERFACE_PROFILE=1"]
    for kv in args.env or []:
        env += ["--env", kv]
    subprocess.run(["open", "-a", "OBS", *env], check=True)
    print("websocket up" if wait_ws() else "websocket did not come up")


def parse_value(v: str):
    if v.lower() in ("true", "false"):
        return v.lower() == "true"
    try:
        return float(v)
    except ValueError:
        return v


def cmd_set(args):
    cl = client()
    upd = {}
    for kv in args.pairs:
        k, v = kv.split("=", 1)
        val = parse_value(v)
        upd[k] = int(val) if isinstance(val, float) and val.is_integer() else val
    cl.set_source_filter_settings(INPUT, FILTER, upd, True)
    print("set", upd)


def source_size(cl) -> tuple[int, int]:
    item = cl.get_scene_item_id(SCENE, INPUT).scene_item_id
    t = cl.get_scene_item_transform(SCENE, item).scene_item_transform
    return int(t["sourceWidth"]), int(t["sourceHeight"])


def save_shot(cl, path: Path):
    w, h = source_size(cl)
    r = cl.get_source_screenshot(INPUT, "png", w, h, -1)
    data = r.image_data.split(",", 1)[1]
    path.write_bytes(base64.b64decode(data))


def cmd_shot(args):
    cl = client()
    SHOTS.mkdir(exist_ok=True)
    if args.at is not None:
        cl.trigger_media_input_action(INPUT, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PAUSE")
        cl.set_media_input_cursor(INPUT, int(args.at * 1000))
        time.sleep(args.settle)
    cl.set_source_filter_enabled(INPUT, FILTER, True)
    time.sleep(args.settle)
    save_shot(cl, SHOTS / f"{args.name}_on.png")
    if not args.no_off:
        cl.set_source_filter_enabled(INPUT, FILTER, False)
        time.sleep(0.4)
        save_shot(cl, SHOTS / f"{args.name}_off.png")
        cl.set_source_filter_enabled(INPUT, FILTER, True)
    if args.play:
        cl.trigger_media_input_action(INPUT, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PLAY")
    print("saved", args.name)


def cmd_stats(args):
    cl = client()
    s = cl.get_stats()
    keys = ["active_fps", "average_frame_render_time", "render_skipped_frames",
            "render_total_frames", "output_skipped_frames", "cpu_usage"]
    print({k: getattr(s, k) for k in keys})


def latest_log() -> Path:
    return max(LOG_DIR.glob("*.txt"), key=lambda p: p.stat().st_mtime)


def cmd_log(args):
    lines = latest_log().read_text(errors="replace").splitlines()
    hits = [ln for ln in lines if "betterface" in ln.lower() or "effect" in ln.lower() and "error" in ln.lower()]
    print("\n".join(hits[-40:]) or "nothing logged")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("setup"); p.add_argument("video"); p.set_defaults(fn=cmd_setup)
    p = sub.add_parser("load"); p.set_defaults(fn=cmd_load)
    p = sub.add_parser("restart"); p.add_argument("--env", action="append", help="KEY=VALUE for OBS")
    p.set_defaults(fn=cmd_restart)
    p = sub.add_parser("set"); p.add_argument("pairs", nargs="+"); p.set_defaults(fn=cmd_set)
    p = sub.add_parser("shot"); p.add_argument("name"); p.add_argument("--at", type=float)
    p.add_argument("--settle", type=float, default=1.2)
    p.add_argument("--no-off", action="store_true"); p.add_argument("--play", action="store_true")
    p.set_defaults(fn=cmd_shot)
    p = sub.add_parser("stats"); p.set_defaults(fn=cmd_stats)
    p = sub.add_parser("log"); p.set_defaults(fn=cmd_log)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
