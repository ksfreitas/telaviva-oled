#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/SessionLocker.swift" \
  "$ROOT/native/macos/TelaViva/LockCoordinator.swift" \
  "$ROOT/native/macos/Tests/SessionLockTests.swift" \
  -framework Foundation \
  -o "$TEMP_DIR/session-lock-tests"
"$TEMP_DIR/session-lock-tests"
