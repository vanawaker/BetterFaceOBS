#!/usr/bin/env python3
"""Record the test clip once per configuration through OBS's own recorder.

record_ab.py VIDEO TAG [--fps N] CONFIG...
CONFIG is "off" (filter disabled) or "name:key=value,key=value" (filter enabled with those settings).
Output: testmedia/rec/TAG_<name>.mp4, each starting at the beginning of the clip.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obsctl  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("video")
ap.add_argument("tag")
ap.add_argument("configs", nargs="+")
ap.add_argument("--fps", type=int, default=0)
ap.add_argument("--seconds", type=float, default=0)
a = ap.parse_args()

cl = obsctl.client()
if a.fps:
    v = cl.get_video_settings()
    if v.fps_numerator != a.fps or v.fps_denominator != 1:
        cl.set_video_settings(a.fps, 1, v.base_width, v.base_height, v.output_width, v.output_height)
obsctl.cmd_setup(type("A", (), {"video": a.video})())
time.sleep(1.5)
base = dict(cl.get_source_filter(obsctl.INPUT, obsctl.FILTER).filter_settings)
dur = a.seconds or float(cl.get_media_input_status(obsctl.INPUT).media_duration or 10000) / 1000.0 - 0.3
for cfg in a.configs:
    name, _, pairs = cfg.partition(":")
    on = name != "off"
    if on:
        s = dict(base)
        s["debug"] = False
        for kv in filter(None, pairs.split(",")):
            k, val = kv.split("=", 1)
            x = obsctl.parse_value(val)
            s[k] = int(x) if isinstance(x, float) and x.is_integer() else x
        cl.set_source_filter_settings(obsctl.INPUT, obsctl.FILTER, s, False)
    cl.set_source_filter_enabled(obsctl.INPUT, obsctl.FILTER, on)
    cl.trigger_media_input_action(obsctl.INPUT, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PAUSE")
    cl.set_media_input_cursor(obsctl.INPUT, 0)
    time.sleep(1.5)
    cl.start_record()
    time.sleep(0.5)
    cl.trigger_media_input_action(obsctl.INPUT, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PLAY")
    # After pause + seek the source sometimes reports "playing" without advancing; a second play unsticks it
    time.sleep(0.3)
    cl.trigger_media_input_action(obsctl.INPUT, "OBS_WEBSOCKET_MEDIA_INPUT_ACTION_PLAY")
    time.sleep(dur - 0.3)
    path = Path(cl.stop_record().output_path)
    time.sleep(1.5)
    dst = path.with_name(f"{a.tag}_{name}.mp4")
    path.rename(dst)
    print(name, dst)
cl.set_source_filter_settings(obsctl.INPUT, obsctl.FILTER, base, False)
cl.set_source_filter_enabled(obsctl.INPUT, obsctl.FILTER, True)
