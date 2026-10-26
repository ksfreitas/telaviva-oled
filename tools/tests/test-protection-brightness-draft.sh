#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/ProtectionBrightnessDraft.swift" \
  "$ROOT/native/macos/Tests/ProtectionBrightnessDraftTests.swift" \
  -o "$TEMP_DIR/protection-brightness-draft-tests"
"$TEMP_DIR/protection-brightness-draft-tests"
