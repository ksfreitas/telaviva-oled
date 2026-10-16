#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc "$ROOT/native/macos/TelaViva/DDCTransaction.swift" \
  "$ROOT/native/macos/Tests/DDCTransactionTests.swift" \
  -o "$TEMP_DIR/ddc-transaction-tests"
"$TEMP_DIR/ddc-transaction-tests"
