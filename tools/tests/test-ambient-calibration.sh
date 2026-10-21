#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
swiftc "$ROOT/native/macos/TelaViva/BuiltInDisplayBackend.swift" \
  "$ROOT/native/macos/TelaViva/DiagnosticLog.swift" \
  "$ROOT/native/macos/TelaViva/CoreBridge.swift" \
  "$ROOT/native/macos/TelaViva/ManualMonitorControls.swift" \
  "$ROOT/native/macos/TelaViva/ControlsBridge.swift" \
  "$ROOT/native/macos/TelaViva/ControlsDriver.swift" \
  "$ROOT/native/macos/TelaViva/HardwareKeyRouter.swift" \
  "$ROOT/native/macos/TelaViva/AutoBrightnessHolding.swift" \
  "$ROOT/native/macos/TelaViva/BuiltInAutoBrightnessOwner.swift" \
  "$ROOT/native/macos/TelaViva/AmbientCalibrationController.swift" \
  "$ROOT/native/macos/Tests/AmbientCalibrationTests.swift" \
  -o "$TMP/ambient-calibration-tests"
"$TMP/ambient-calibration-tests"
