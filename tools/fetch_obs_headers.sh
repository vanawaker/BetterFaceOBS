#!/bin/zsh
# Fetch only the libobs headers used by the build, without replacing an existing checkout.
set -euo pipefail
ROOT=${0:A:h:h}
DEST=$ROOT/third_party/obs-studio
EXPECTED=0b2c85845e20f41c612cfe2583e380bd436b6b3c
if [[ -e $DEST ]]; then
  [[ $(git -C "$DEST" rev-parse HEAD) == $EXPECTED ]] || { print -u2 'Existing OBS checkout has a different revision'; exit 1; }
  [[ -f $DEST/libobs/obs-module.h ]] || { print -u2 'Existing OBS checkout is missing headers'; exit 1; }
  print 'OBS headers already available'
  exit 0
fi
mkdir -p "$DEST:h"
git clone --depth 1 --filter=blob:none --sparse --branch 31.0.0 https://github.com/obsproject/obs-studio.git "$DEST"
git -C "$DEST" sparse-checkout set libobs
[[ $(git -C "$DEST" rev-parse HEAD) == $EXPECTED ]] || { print -u2 'Unexpected OBS header revision'; exit 1; }
print 'OBS headers verified'
