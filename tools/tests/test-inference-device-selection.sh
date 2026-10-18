#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
swiftc "$ROOT/native/macos/TelaViva/InferenceDeviceSelection.swift" \
  "$ROOT/native/macos/Tests/InferenceDeviceSelectionTests.swift" \
  -o "$TMP/inference-device-selection-tests"
"$TMP/inference-device-selection-tests"
