#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST="$SCRIPT_DIR/runtime/macos-arm64/libonnxruntime.dylib"
URL='https://github.com/microsoft/onnxruntime/releases/download/v1.23.0/onnxruntime-osx-arm64-1.23.0.tgz'
ARCHIVE_SHA256='8182db0ebb5caa21036a3c78178f17fabb98a7916bdab454467c8f4cf34bcfdf'
DYLIB_SHA256='d3859aecdb70ea099f5b5f4185fe16f0527c6680b18731e6e96fc971ec767cca'

if [ "$#" -eq 1 ] && [ "$1" = "--check" ]; then
    if [ ! -f "$DEST" ]; then
        printf 'Missing ONNX Runtime at %s; fetch it with %s\n' "$DEST" "$0" >&2
        exit 1
    fi
    printf '%s  %s\n' "$DYLIB_SHA256" "$DEST" | shasum -a 256 -c -
    exit 0
fi
if [ "$#" -ne 0 ]; then
    printf 'Usage: %s [--check]\n' "$0" >&2
    exit 2
fi

TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

curl --fail --location --silent --show-error "$URL" -o "$TEMP_DIR/onnxruntime.tgz"
printf '%s  %s\n' "$ARCHIVE_SHA256" "$TEMP_DIR/onnxruntime.tgz" | shasum -a 256 -c -
tar -xzf "$TEMP_DIR/onnxruntime.tgz" -C "$TEMP_DIR"
mkdir -p "$(dirname -- "$DEST")"
cp "$TEMP_DIR/onnxruntime-osx-arm64-1.23.0/lib/libonnxruntime.1.23.0.dylib" "$DEST"
printf '%s  %s\n' "$DYLIB_SHA256" "$DEST" | shasum -a 256 -c -
printf 'Installed verified ONNX Runtime to %s\n' "$DEST"
