#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/native/macos/TelaViva"
OUT="${1:-$ROOT/build/macos}"
APP="$OUT/TelaViva OLED.app"
CORE_LIB="${TELAVIVA_CORE_LIB:-$ROOT/target/release/libtelaviva_core.a}"
INFERENCE_LIB="${TELAVIVA_INFERENCE_LIB:-$ROOT/target/release/libtelaviva_inference.a}"
for required in "$CORE_LIB" "$INFERENCE_LIB"; do
  if [[ ! -f "$required" ]]; then
    echo "Required native input is missing: $required" >&2
    exit 2
  fi
done
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources/models"
cp "$SRC/Info.plist" "$APP/Contents/Info.plist"
mkdir -p "$APP/Contents/Resources/en.lproj" "$APP/Contents/Resources/pt-BR.lproj"
cp "$SRC/en.lproj/InfoPlist.strings" "$APP/Contents/Resources/en.lproj/InfoPlist.strings"
cp "$SRC/pt-BR.lproj/InfoPlist.strings" "$APP/Contents/Resources/pt-BR.lproj/InfoPlist.strings"
python3 "$ROOT/tools/inference/prepare.py" \
  --resources-dir "$APP/Contents/Resources" \
  --frameworks-dir "$APP/Contents/Frameworks"

LICENSES="$APP/Contents/Resources/ThirdPartyLicenses"
mkdir -p "$LICENSES"
ditto "$ROOT/licenses" "$LICENSES/licenses"
cp "$ROOT/native/macos/DDC-LICENSE" "$LICENSES/DDC-LICENSE"
cp "$ROOT/LICENSE" "$APP/Contents/Resources/LICENSE"

SDK="$(xcrun --sdk macosx --show-sdk-path)"
SOURCES=("$SRC/App.swift" "$SRC/CoreBridge.swift" "$SRC/CameraLifecycle.swift" "$SRC/CameraSelection.swift" "$SRC/HardwareKeyRouter.swift" "$SRC/MenuBarController.swift" "$SRC/MonitorPopup.swift" "$SRC/SettingsView.swift" "$SRC/PresenceCamera.swift" "$SRC/Localization.swift" "$SRC/MacDDCController.swift" "$SRC/DDCTransaction.swift" "$SRC/ManualMonitorControls.swift" "$SRC/DefaultAudioOutputMonitor.swift" "$SRC/MediaKeyProbe.swift")
swiftc -sdk "$SDK" -target arm64-apple-macos14.0 -module-cache-path "$OUT/module-cache" \
  -O \
  -import-objc-header "$SRC/TelaViva-Bridging-Header.h" -I "$ROOT/include" -Xcc -I -Xcc "$ROOT/include" \
  -D TELAVIVA_CORE -D TELAVIVA_INFERENCE \
  -framework AppKit -framework SwiftUI -framework IOKit -framework ServiceManagement -framework AVFoundation -framework CoreGraphics -framework CoreAudio \
  "${SOURCES[@]}" "$CORE_LIB" "$INFERENCE_LIB" -lc++ -lz \
  -Xlinker -rpath -Xlinker '@executable_path/../Frameworks' -o "$APP/Contents/MacOS/TelaViva"

# Local ad-hoc builds only: an identifier-based designated requirement keeps
# Accessibility and Input Monitoring grants valid across local rebuilds.
# Distribution signing and notarization remain separate.
find "$APP/Contents/Frameworks" -name '*.dylib' -print0 | while IFS= read -r -d '' f; do
  codesign --force --sign - "$f"
done
codesign --force --sign - -r='designated => identifier "com.telaviva.oled"' "$APP"
echo "Built ad-hoc signed local app bundle: $APP"
