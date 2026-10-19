#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
swiftc "$ROOT/native/macos/TelaViva/CameraFrameRate.swift" \
  "$ROOT/native/macos/Tests/CameraFrameRateTests.swift" \
  -o "$TMP/camera-frame-rate-tests"
"$TMP/camera-frame-rate-tests"
swiftc "$ROOT/native/macos/TelaViva/CameraSelection.swift" \
  "$ROOT/native/macos/Tests/CameraSelectionTests.swift" \
  -o "$TMP/camera-selection-tests"
"$TMP/camera-selection-tests"
swiftc "$ROOT/native/macos/TelaViva/DiagnosticLog.swift" \
  "$ROOT/native/macos/TelaViva/CoreBridge.swift" \
  "$ROOT/native/macos/TelaViva/ManualMonitorControls.swift" \
  "$ROOT/native/macos/TelaViva/ControlsBridge.swift" \
  "$ROOT/native/macos/TelaViva/ControlsDriver.swift" \
  "$ROOT/native/macos/TelaViva/HardwareKeyRouter.swift" \
  "$ROOT/native/macos/TelaViva/Localization.swift" \
  "$ROOT/native/macos/Tests/CameraLocalizationTests.swift" \
  -o "$TMP/camera-localization-tests"
"$TMP/camera-localization-tests"
