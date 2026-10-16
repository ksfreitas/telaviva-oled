#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc \
  "$ROOT/native/macos/TelaViva/HardwareKeyRouter.swift" \
  "$ROOT/native/macos/TelaViva/CoreBridge.swift" \
  "$ROOT/native/macos/TelaViva/ManualMonitorControls.swift" \
  "$ROOT/native/macos/Tests/ManualMonitorControlsTests.swift" \
  -o "$TEMP_DIR/manual-monitor-controls-tests"
"$TEMP_DIR/manual-monitor-controls-tests"
