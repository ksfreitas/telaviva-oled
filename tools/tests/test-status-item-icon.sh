#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/StatusItemIcon.swift" \
  "$ROOT/native/macos/TelaViva/CoreBridge.swift" \
  "$ROOT/native/macos/TelaViva/ManualMonitorControls.swift" \
  "$ROOT/native/macos/TelaViva/ControlsBridge.swift" \
  "$ROOT/native/macos/TelaViva/ControlsDriver.swift" \
  "$ROOT/native/macos/TelaViva/HardwareKeyRouter.swift" \
  "$ROOT/native/macos/TelaViva/Localization.swift" \
  "$ROOT/native/macos/Tests/StatusItemIconTests.swift" \
  -o "$TEMP_DIR/status-item-icon-tests"
"$TEMP_DIR/status-item-icon-tests"
