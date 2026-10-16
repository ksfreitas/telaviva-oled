# macOS local build

The local macOS build targets Apple Silicon and macOS 14 or later. It produces
an ad-hoc signed `TelaViva OLED.app`; this signature supports local execution
and does not provide Developer ID distribution or notarization.

## Prerequisites

- Xcode command line tools and the macOS 14 SDK
- Rust toolchain with the `aarch64-apple-darwin` target
- Python 3
- The repository-pinned YuNet model and ONNX Runtime files. Fetch the runtime
  with `tools/inference/fetch-macos-runtime.sh`; the download and library hashes
  are checked, and the packaging helper checks both assets again.

## Build

From the repository root:

```sh
cargo build --workspace --release --target aarch64-apple-darwin
TARGET_DIR="$(cargo metadata --no-deps --format-version 1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["target_directory"])')"
TELAVIVA_CORE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_core.a" \
TELAVIVA_INFERENCE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_inference.a" \
  packaging/macos/build-app.sh
```

The resulting app is `build/macos/TelaViva OLED.app`. Pass a different output
directory as the script's first argument to place it elsewhere. Full third-party
license texts, including ONNX Runtime notices and the DDC transport attribution,
are included in `Contents/Resources/ThirdPartyLicenses/`. The app's MIT license
is included separately at `Contents/Resources/LICENSE`.

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
