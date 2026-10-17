#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/BuiltInDisplayBackend.swift" \
  "$ROOT/native/macos/TelaViva/BuiltInBrightnessMonitor.swift" \
  "$ROOT/native/macos/Tests/BuiltInBrightnessMonitorTests.swift" \
  -o "$TEMP_DIR/builtin-brightness-monitor-tests"
"$TEMP_DIR/builtin-brightness-monitor-tests"
