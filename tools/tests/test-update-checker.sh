#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/UpdateChecker.swift" \
  "$ROOT/native/macos/Tests/UpdateCheckerTests.swift" \
  -o "$TEMP_DIR/update-checker-tests"
"$TEMP_DIR/update-checker-tests"
