#!/bin/zsh
# Build build/track_replay (the plugin's FaceTracker run over a video file, see tools/track_replay.mm)
# and build/warpcheck (slim warp continuity from a dump, see tools/warpcheck.cpp).
set -euo pipefail
ROOT=${0:A:h:h}
INC=${OBS_INCLUDE_DIR:-$ROOT/third_party/obs-studio/libobs}
mkdir -p $ROOT/build
clang++ -std=c++17 -fobjc-arc -arch arm64 -mmacosx-version-min=13.0 -O2 -g0 -Wall -I$INC \
  $ROOT/tools/track_replay.mm $ROOT/plugin/src/tracker.mm $ROOT/plugin/src/geometry.cpp \
  -o $ROOT/build/track_replay \
  -framework Foundation -framework AVFoundation -framework CoreMedia -framework CoreVideo \
  -framework Vision -framework CoreGraphics -framework Accelerate
echo "built $ROOT/build/track_replay"
clang++ -std=c++17 -arch arm64 -mmacosx-version-min=13.0 -O2 -Wall -I$ROOT/plugin/src \
  $ROOT/tools/warpcheck.cpp $ROOT/plugin/src/geometry.cpp -o $ROOT/build/warpcheck
echo "built $ROOT/build/warpcheck"
