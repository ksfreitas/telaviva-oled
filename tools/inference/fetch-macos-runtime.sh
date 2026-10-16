#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$SCRIPT_DIR/runtime/macos-arm64/libonnxruntime.dylib"
URL='https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-osx-arm64-1.20.1.tgz'
ARCHIVE_SHA256='b678fc3c2354c771fea4fba420edeccfba205140088334df801e7fc40e83a57a'
DYLIB_SHA256='530cdb5a0de774677d369a83ec8912b0242e9769acd12d34b61412ee6ae368ae'
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

curl --fail --location --silent --show-error "$URL" -o "$TEMP_DIR/onnxruntime.tgz"
printf '%s  %s\n' "$ARCHIVE_SHA256" "$TEMP_DIR/onnxruntime.tgz" | shasum -a 256 -c -
tar -xzf "$TEMP_DIR/onnxruntime.tgz" -C "$TEMP_DIR"
mkdir -p "$(dirname -- "$DEST")"
cp "$TEMP_DIR/onnxruntime-osx-arm64-1.20.1/lib/libonnxruntime.1.20.1.dylib" "$DEST"
printf '%s  %s\n' "$DYLIB_SHA256" "$DEST" | shasum -a 256 -c -
printf 'Installed verified ONNX Runtime to %s\n' "$DEST"
