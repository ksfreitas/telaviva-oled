#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
swiftc "$ROOT/native/macos/TelaViva/AutoBrightnessHolding.swift" \
  "$ROOT/native/macos/TelaViva/BuiltInDisplayBackend.swift" \
  "$ROOT/native/macos/TelaViva/DiagnosticLog.swift" \
  "$ROOT/native/macos/TelaViva/BuiltInAutoBrightnessOwner.swift" \
  "$ROOT/native/macos/Tests/BuiltInAutoBrightnessOwnerTests.swift" \
  -o "$TMP/builtin-auto-brightness-owner-tests"
"$TMP/builtin-auto-brightness-owner-tests"
