#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/PopupLayout.swift" \
  "$ROOT/native/macos/Tests/PopupLayoutTests.swift" \
  -o "$TEMP_DIR/popup-layout-tests"
"$TEMP_DIR/popup-layout-tests"
