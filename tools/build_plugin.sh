#!/bin/zsh
# Build build/BetterFace.plugin (arm64) against the installed OBS.app's libobs.
set -euo pipefail
ROOT=${0:A:h:h}
VERSION=${1:-2.0.5}
OBS_APP=${OBS_APP:-/Applications/OBS.app}
OBS_FW=$OBS_APP/Contents/Frameworks
INC=${OBS_INCLUDE_DIR:-$ROOT/third_party/obs-studio/libobs}
OUT=${2:-$ROOT/build/BetterFace.plugin}
OBJ=${OUT:h}/${OUT:t}.objects
mkdir -p $OBJ $OUT/Contents/MacOS $OUT/Contents/Resources

CFLAGS=(-arch arm64 -mmacosx-version-min=13.0 -O2 -g0 -Wall -Wno-unused-parameter
        "-ffile-prefix-map=$ROOT=/src/betterface" "-fdebug-prefix-map=$ROOT=/src/betterface"
        "-ffile-prefix-map=$HOME=/build/home" "-fdebug-prefix-map=$HOME=/build/home"
        -I$INC -DBETTERFACE_VERSION=\"$VERSION\")
if [[ ${BETTERFACE_TEST_FAULTS:-0} == 1 ]]; then
  CFLAGS+=(-DBETTERFACE_TEST_FAULTS=1)
fi
srcs=(plugin-main.cpp betterface-filter.cpp geometry.cpp frame-convert.cpp tracker.mm)
objs=()
for s in $srcs; do
  o=$OBJ/${s%.*}.o
  case $s in
    *.mm)  clang++ -std=c++17 -fobjc-arc $CFLAGS -c $ROOT/plugin/src/$s -o $o ;;
    *.cpp) clang++ -std=c++17 $CFLAGS -c $ROOT/plugin/src/$s -o $o ;;
  esac
  objs+=$o
done
clang++ -arch arm64 -mmacosx-version-min=13.0 -bundle $objs -o $OUT/Contents/MacOS/BetterFace \
  -F$OBS_FW -framework libobs -Wl,-rpath,@executable_path/../Frameworks \
  -framework Foundation -framework CoreVideo -framework Vision -framework CoreGraphics
# Remove linker debug records, including object-file paths, before signing.
strip -S "$OUT/Contents/MacOS/BetterFace"
sed "s/@VERSION@/$VERSION/g" $ROOT/plugin/Info.plist > $OUT/Contents/Info.plist
rsync -a --delete $ROOT/plugin/data/ $OUT/Contents/Resources/
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$OUT/Contents/Resources/"
codesign --force --sign - $OUT >/dev/null 2>&1
echo "built $OUT ($VERSION)"
