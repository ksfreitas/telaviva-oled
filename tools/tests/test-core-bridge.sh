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
  "$ROOT/native/macos/TelaViva/HardwareKeyRouter.swift" \
  "$ROOT/native/macos/TelaViva/DiagnosticLog.swift" \
  "$ROOT/native/macos/TelaViva/CoreBridge.swift" \
  "$ROOT/native/macos/TelaViva/UpdateChecker.swift" \
  "$ROOT/native/macos/TelaViva/ManualMonitorControls.swift" \
  "$ROOT/native/macos/TelaViva/ControlsBridge.swift" \
  "$ROOT/native/macos/TelaViva/ControlsDriver.swift" \
  "$ROOT/native/macos/Tests/CoreBridgeTests.swift" \
  "$CORE_LIB" -lc++ -lz \
  -o "$TEMP_DIR/core-bridge-tests"
"$TEMP_DIR/core-bridge-tests"
