#!/bin/sh
# Compiles the TelaViva OLED app icon into macOS icon resources.
#
# Purpose: builds Assets.car from the Icon Composer document
# assets/branding/AppIcon.icon and renders the fallback AppIcon.icns from the
# legacy artwork assets/branding/app-icon.svg. Both are copied into
# packaging/macos/Resources. The .icns is rendered at all ten iconset sizes
# because the .icns that actool emits only contains 16 and 128 px sizes.
# The outputs are committed because CI runs on macos-14, which cannot compile
# .icon documents.
#
# Requirements: macOS with Xcode 26 or later, which provides xcrun actool and
# xcrun swiftc. Before compiling, the script checks that the layer files in
# assets/branding/liquid-glass/ are byte-identical to the copies in
# assets/branding/AppIcon.icon/Assets/.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if ! command -v xcrun >/dev/null 2>&1; then
  echo "error: xcrun not found; install Xcode 26 or later with actool" >&2
  exit 1
fi

for layer in background.svg screen.svg stand.svg dot.svg; do
  if ! cmp -s "$root/assets/branding/liquid-glass/$layer" "$root/assets/branding/AppIcon.icon/Assets/$layer"; then
    echo "error: $layer differs between assets/branding/liquid-glass/ and assets/branding/AppIcon.icon/Assets/" >&2
    exit 1
  fi
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

xcrun actool --compile "$tmp" --platform macosx --minimum-deployment-target 14.0 --app-icon AppIcon --output-partial-info-plist "$tmp/partial.plist" --output-format human-readable-text "$root/assets/branding/AppIcon.icon" >/dev/null

if [ ! -f "$tmp/Assets.car" ]; then
  echo "error: actool did not produce Assets.car" >&2
  exit 1
fi

xcrun swiftc -O "$root/tools/branding/render-svg.swift" -o "$tmp/render-svg"

iconset="$tmp/AppIcon.iconset"
mkdir -p "$iconset"
for entry in \
  icon_16x16.png:16 icon_16x16@2x.png:32 \
  icon_32x32.png:32 icon_32x32@2x.png:64 \
  icon_128x128.png:128 icon_128x128@2x.png:256 \
  icon_256x256.png:256 icon_256x256@2x.png:512 \
  icon_512x512.png:512 icon_512x512@2x.png:1024; do
  name=${entry%%:*}
  px=${entry##*:}
  "$tmp/render-svg" "$root/assets/branding/app-icon.svg" "$iconset/$name" "$px"
done
iconutil -c icns "$iconset" -o "$tmp/AppIcon.icns"

dest="$root/packaging/macos/Resources"
mkdir -p "$dest"
cp "$tmp/Assets.car" "$tmp/AppIcon.icns" "$dest/"

echo "wrote $dest/Assets.car"
echo "wrote $dest/AppIcon.icns"
