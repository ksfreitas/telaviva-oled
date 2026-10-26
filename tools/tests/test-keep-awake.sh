#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
swiftc "$ROOT/native/macos/TelaViva/KeepAwakeController.swift" \
  "$ROOT/native/macos/Tests/KeepAwakeTests.swift" \
  -framework AppKit -framework ApplicationServices -framework IOKit \
  -o "$TMP/keep-awake-tests"
"$TMP/keep-awake-tests"
