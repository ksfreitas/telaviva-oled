# macOS local build

The local macOS build targets Apple Silicon and macOS 14 or later. It produces
an ad-hoc signed `TelaViva OLED.app`; this signature supports local execution
and does not provide Developer ID distribution or notarization.

## Prerequisites

- Xcode command line tools and the macOS 14 SDK
- Rust toolchain
- Python 3
- The repository-pinned YuNet model. The `scripts/build-macos.sh` helper fetches the
  pinned ONNX Runtime on the first build, checks its hashes, and reuses it on
  later builds. Packaging verifies both inference assets again.

## Build

For the one-command build, run `./scripts/build-macos.sh` from the repository root. It
creates `build/macos/TelaViva OLED.app`, installs the Rust target if needed,
builds the workspace, and packages the app. The first build needs network access
to download the pinned runtime. The lower-level steps are:

```sh
cargo build --workspace --release --target aarch64-apple-darwin --locked
TARGET_DIR="$(cargo metadata --locked --no-deps --format-version 1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["target_directory"])')"
TELAVIVA_CORE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_core.a" \
TELAVIVA_INFERENCE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_inference.a" \
  packaging/macos/build-app.sh
```

The resulting app is `build/macos/TelaViva OLED.app`. Full third-party
license texts, including ONNX Runtime notices and the DDC transport attribution,
are included in `Contents/Resources/ThirdPartyLicenses/`. The app's MIT license
is included separately at `Contents/Resources/LICENSE`.

## Permissions

Keyboard routing needs both Accessibility and Input Monitoring under System
Settings > Privacy & Security.

Grants made for builds signed before this change are tied to the per-build
hash. Remove the app from each list (minus button) and add it again once.
Later rebuilds keep the grants because the designated requirement is
identifier-based (`identifier "com.telaviva.oled"`).

An identifier-only requirement means any ad-hoc app claiming the same
identifier would satisfy it. That is acceptable only for local development
builds; release builds should use a real Developer ID signature.

## Diagnostics

Run these from the built app's executable. `--list-displays` performs actual
read-only DDC/CI reads of brightness (VCP `0x10`) and volume (`0x62`); it does
not change monitor settings or access the camera. An empty list means no
external display was discovered through the supported IORegistry path. A listed
display can still report either VCP feature as unsupported.

```sh
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --smoke-test
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --list-displays
'build/macos/TelaViva OLED.app/Contents/MacOS/TelaViva' --diagnostics
```

The diagnostics command includes CoreGraphics discovery counts, IORegistry
adapter and IOAV transport match results, and per-display stage information.

The smoke test exercises creation, snapshot, invalid-input rejection, and a
valid core event without opening the UI or camera.

### Key probe

`--probe-media-keys [seconds]` lists which events a keyboard's brightness,
volume, and mute keys produce (default 20 seconds, clamped to 5-60). Launch the
app so macOS attributes the event tap to it, press the keys during the window,
then read the file:

```sh
open -n --stdout /tmp/telaviva-keys.txt 'build/macos/TelaViva OLED.app' --args --probe-media-keys 20
cat /tmp/telaviva-keys.txt
```

Only media-control events and function-key codes (F1-F20 and a few media key
codes) are listed. Letters, digits, and text are never recorded, and nothing is
written except to standard output. The probe needs the app's Accessibility and
Input Monitoring grants (it prints which one is missing and exits with code 2).
It is listen-only: it never modifies, consumes, or injects events.
