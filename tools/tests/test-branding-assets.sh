#!/bin/sh
# Checks the branding assets and the app icon wiring without running the icon build.
#
# Verifies that every SVG under assets/branding is well-formed XML, that the
# Icon Composer document parses and references existing layers, that the
# liquid-glass copies match the document's layers, that the committed icon
# resources are present, that AppIcon.icns has the ten expected iconset sizes,
# that Info.plist names AppIcon, and that the menubar icons exist.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

BRANDING="$ROOT/assets/branding"
ICON_DOC="$BRANDING/AppIcon.icon"
RESOURCES="$ROOT/packaging/macos/Resources"

# Every SVG under assets/branding must be well-formed XML.
find "$BRANDING" -name '*.svg' > "$TEMP_DIR/svgs"
[ -s "$TEMP_DIR/svgs" ] || fail "no SVG files found under assets/branding"
while IFS= read -r svg; do
  xmllint --noout "$svg" || fail "$svg is not well-formed XML"
done < "$TEMP_DIR/svgs"

# icon.json must parse, and every image-name it references must exist.
python3 -I -c 'import json,sys; json.load(open(sys.argv[1]))' "$ICON_DOC/icon.json" \
  || fail "AppIcon.icon/icon.json does not parse"
python3 -I -c '
import json, sys
doc = json.load(open(sys.argv[1]))
for group in doc["groups"]:
    for layer in group["layers"]:
        print(layer["image-name"])
' "$ICON_DOC/icon.json" > "$TEMP_DIR/image-names" \
  || fail "could not read image-name entries from AppIcon.icon/icon.json"
[ -s "$TEMP_DIR/image-names" ] || fail "AppIcon.icon/icon.json references no image-name entries"
while IFS= read -r name; do
  [ -f "$ICON_DOC/Assets/$name" ] || fail "icon.json image-name $name is missing from AppIcon.icon/Assets/"
done < "$TEMP_DIR/image-names"

# Each liquid-glass layer must be byte-identical to the copy in the icon document.
layer_count=0
for layer in "$BRANDING/liquid-glass"/*; do
  [ -f "$layer" ] || continue
  layer_count=$((layer_count + 1))
  name=${layer##*/}
  cmp -s "$layer" "$ICON_DOC/Assets/$name" \
    || fail "liquid-glass/$name differs from AppIcon.icon/Assets/$name"
done
[ "$layer_count" -gt 0 ] || fail "no layer files found in assets/branding/liquid-glass/"

# Committed icon resources from the build script.
[ -s "$RESOURCES/Assets.car" ] || fail "packaging/macos/Resources/Assets.car is missing or empty"
[ -f "$RESOURCES/AppIcon.icns" ] || fail "packaging/macos/Resources/AppIcon.icns is missing"

# AppIcon.icns must contain exactly the ten expected iconset entries.
iconutil -c iconset "$RESOURCES/AppIcon.icns" -o "$TEMP_DIR/x.iconset" \
  || fail "iconutil could not read packaging/macos/Resources/AppIcon.icns"
for name in \
  icon_16x16.png icon_16x16@2x.png \
  icon_32x32.png icon_32x32@2x.png \
  icon_128x128.png icon_128x128@2x.png \
  icon_256x256.png icon_256x256@2x.png \
  icon_512x512.png icon_512x512@2x.png; do
  echo "$name"
done | sort > "$TEMP_DIR/expected-iconset"
ls -1 "$TEMP_DIR/x.iconset" | sort > "$TEMP_DIR/actual-iconset"
cmp -s "$TEMP_DIR/expected-iconset" "$TEMP_DIR/actual-iconset" \
  || fail "AppIcon.icns does not contain exactly the ten expected iconset files"

# Info.plist must point the bundle at AppIcon.
PLIST="$ROOT/native/macos/TelaViva/Info.plist"
[ "$(plutil -extract CFBundleIconFile raw -o - "$PLIST")" = "AppIcon" ] \
  || fail "Info.plist CFBundleIconFile is not AppIcon"
[ "$(plutil -extract CFBundleIconName raw -o - "$PLIST")" = "AppIcon" ] \
  || fail "Info.plist CFBundleIconName is not AppIcon"

# Menubar status icons.
for svg in presence-dark.svg presence-detected.svg presence-absent.svg; do
  [ -f "$BRANDING/menubar/$svg" ] || fail "menubar icon $svg is missing"
done

echo "PASS: branding assets"
