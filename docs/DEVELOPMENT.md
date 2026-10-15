# Development

## Components

- `plugin/src/tracker.mm`: Apple Vision face detection, tracking, landmarks, and person segmentation.
- `plugin/src/geometry.cpp`: face geometry and skin masks.
- `plugin/src/frame-convert.cpp`: input-frame conversion.
- `plugin/src/betterface-filter.cpp`: OBS callbacks, GPU passes, and failure bypass.
- `plugin/data/betterface.effect`: skin processing and geometric warp shaders.

Source frames and inference remain in the local process. The plugin does not implement a network
client. Optional diagnostic dumps are enabled only through `BETTERFACE_DUMP`; they contain facial
landmarks and should remain private unless reviewed and intentionally shared.

## Dependencies

OBS headers are fetched from the official OBS repository at tag `31.0.0`, commit
`0b2c85845e20f41c612cfe2583e380bd436b6b3c`. Keep the upstream license with the header checkout.
The compiled plugin links to libobs inside the installed OBS application and system frameworks.
The tracker uses the system Vision API rather than a separately distributed model.
BetterFace is licensed under GPL-2.0-or-later. See LICENSE and THIRD_PARTY_NOTICES.md;
both documents are copied into the installed bundle resources.

## Local test tools

Optional Python dependencies: `obsws-python`, `numpy`, and `Pillow`; video comparisons also need
FFmpeg. Use a separate OBS test profile and scene collection. Test scripts can change settings,
recreate filters and sources, start recording, or restart OBS; never run them during a live show.
Provide your own redistributable or private test clips. No captured footage is included here.

Configure local obs-websocket credentials using `OBS_WEBSOCKET_PASSWORD`, or the gitignored
`.obsws_password` file at the checkout root. Do not put a real password in commands, examples,
source files, logs, or Git commits. The helper connects only to localhost.

```sh
python3 tools/obsctl.py setup testmedia/duo.mp4
python3 tools/obsctl.py set smooth=60 slim=30
python3 tools/obsctl.py shot comparison --at 4 --settle 2.5
```

`obsctl.py load` copies shader data into the installed plugin. A binary rebuild alone does not
update the installed version. Install candidates only with OBS closed and verify the loaded
version in the OBS log.

`tools/build_replay.sh` builds offline landmark and warp-analysis tools. `BETTERFACE_PROFILE=1`
enables timing logs. Test-only `BETTERFACE_TEST_FAULTS=1` builds accept hidden fault injection
settings and must never be packaged for users.

The fault test expects a dedicated collection named `BetterFace Safety Test`, scene `Stability Test`,
media input `Audit Portrait`, normal filter `BetterFace Audit`, and a loopback RTMP endpoint at
`rtmp://127.0.0.1:19358/live`. It requires a `2.0.5-test` plugin, an active local test stream, and
no recording. It creates and removes its own `Safety Injection Test` filter.

## Rendering constraints

OBS clears effect parameters after each technique; set all uniforms for every pass. Preserve blend,
framebuffer, and linear-sRGB state before handing control back to OBS. Background protection must
remain disabled until temporal alignment and compositing have independent validation.

Compare face tracking and geometry under motion, turns, occlusions, and different face sizes.
Use static comparisons for image regressions and moving video for temporal behavior. Keep test
results separate from portable source archives and review any screenshots before publishing them.
