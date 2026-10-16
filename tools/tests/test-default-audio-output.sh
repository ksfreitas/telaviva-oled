#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM
swiftc \
  "$ROOT/native/macos/TelaViva/DefaultAudioOutputMonitor.swift" \
  "$ROOT/native/macos/Tests/DefaultAudioOutputMonitorTests.swift" \
  -framework CoreAudio \
  -o "$TEMP_DIR/default-audio-output-tests"
"$TEMP_DIR/default-audio-output-tests"
