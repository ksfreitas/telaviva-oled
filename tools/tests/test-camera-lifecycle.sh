#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
swiftc "$ROOT/native/macos/TelaViva/CameraLifecycle.swift" \
  "$ROOT/native/macos/Tests/CameraLifecycleTests.swift" \
  -o "$TMP/camera-lifecycle-tests"
"$TMP/camera-lifecycle-tests"
swiftc "$ROOT/native/macos/TelaViva/CoreBridge.swift" \
  "$ROOT/native/macos/TelaViva/ManualMonitorControls.swift" \
  "$ROOT/native/macos/TelaViva/HardwareKeyRouter.swift" \
  "$ROOT/native/macos/TelaViva/Localization.swift" \
  "$ROOT/native/macos/Tests/CameraLocalizationTests.swift" \
  -o "$TMP/camera-localization-tests"
"$TMP/camera-localization-tests"
