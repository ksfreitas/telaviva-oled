#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/SettingsWindowLayout.swift" \
  "$ROOT/native/macos/Tests/SettingsWindowLayoutTests.swift" \
  -o "$TEMP_DIR/settings-window-layout-tests"
"$TEMP_DIR/settings-window-layout-tests"
