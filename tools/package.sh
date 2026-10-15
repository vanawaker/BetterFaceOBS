#!/bin/zsh
# Build the plugin and wrap it as dist/BetterFace-for-OBS-<version>.pkg plus the install guide.
# The pkg installs to ~/Library/Application Support/obs-studio/plugins: OBS on Apple Silicon never loads
# plugins from /Library (see tools/distribution.xml). Same layout as obs-plugintemplate.
set -euo pipefail
ROOT=${0:A:h:h}
VERSION=${1:-2.0.5}
cd $ROOT
[[ ${BETTERFACE_TEST_FAULTS:-0} != 1 ]] || { echo "Refusing to package a fault-injection build" >&2; exit 1; }
if [[ $# -ge 2 ]]; then
  INPUT_BUNDLE=$2
else
  ./tools/build_plugin.sh $VERSION
  INPUT_BUNDLE=$ROOT/build/BetterFace.plugin
fi
INPUT_VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$INPUT_BUNDLE/Contents/Info.plist")
[[ $INPUT_VERSION == $VERSION && $VERSION != *test* ]] || { echo "Release/bundle version mismatch" >&2; exit 1; }
PKG=$ROOT/dist/BetterFace-for-OBS-$VERSION.pkg
[[ ! -e $PKG ]] || { echo "Refusing to overwrite $PKG" >&2; exit 1; }

WORK=$(mktemp -d "$ROOT/build/package-$VERSION.XXXXXX")
STAGE=$WORK/pkgroot
mkdir -p "$STAGE/Library/Application Support/obs-studio/plugins"
BUNDLE="$STAGE/Library/Application Support/obs-studio/plugins/BetterFace.plugin"
# No extended attributes: pkgbuild would turn them into ._ files inside the signed bundle
ditto --norsrc --noextattr --noacl "$INPUT_BUNDLE" "$BUNDLE"
xattr -cr "$BUNDLE"
codesign --force --sign - "$BUNDLE" >/dev/null 2>&1
codesign --verify --strict "$BUNDLE"
mkdir -p dist
# Component pkg (paths relative to the install domain) wrapped by productbuild into a current-user-only pkg.
# The bundle must not be relocatable, or Installer "upgrades" any other copy with the same id it finds
# (e.g. a 2.0.0 copy in /Library, or build/ on a dev machine).
TMP=$WORK/pkgtmp
mkdir -p $TMP
pkgbuild --analyze --root $STAGE $TMP/component.plist >/dev/null
plutil -replace 0.BundleIsRelocatable -bool NO $TMP/component.plist
COPYFILE_DISABLE=1 pkgbuild --root $STAGE --filter '(^|/)\._' --filter '(^|/)\.DS_Store$' \
  --component-plist $TMP/component.plist --identifier local.betterface.obs \
  --version $VERSION --install-location / $TMP/BetterFace.pkg >/dev/null
python3 tools/package_clean_metadata.py "$STAGE" "$TMP/BetterFace.pkg"
sed "s/@VERSION@/$VERSION/g" tools/distribution.xml > $TMP/distribution.xml
productbuild --distribution $TMP/distribution.xml --package-path $TMP $PKG >/dev/null
sed "s/@VERSION@/$VERSION/g" dist_template/安装说明.txt > dist/安装说明.txt
sed "s/@VERSION@/$VERSION/g" dist_template/Installation.txt > dist/Installation.txt
pkgutil --payload-files $PKG | head -20
echo "wrote $PKG"
