#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CORE_LIB=${TELAVIVA_CORE_LIB:-"$ROOT/target/aarch64-apple-darwin/release/libtelaviva_core.a"}
if [ ! -f "$CORE_LIB" ]; then
  printf '%s\n' "Missing Rust core library: $CORE_LIB" >&2
  printf '%s\n' "Build it with: cargo build --release --target aarch64-apple-darwin -p telaviva-core" >&2
  exit 2
fi
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc -D TELAVIVA_CORE \
  -import-objc-header "$ROOT/native/macos/TelaViva/TelaViva-Bridging-Header.h" \
  -I "$ROOT/include" \
  "$ROOT/native/macos/TelaViva/CameraLifecycle.swift" \
  "$ROOT/native/macos/Tests/CameraLifecycleBridgeTests.swift" \
  "$CORE_LIB" -lc++ -lz \
  -o "$TEMP_DIR/camera-lifecycle-bridge-tests"
"$TEMP_DIR/camera-lifecycle-bridge-tests"
