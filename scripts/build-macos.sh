#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
  cat <<'EOF'
Usage: ./scripts/build-macos.sh [--help]

Build the Apple Silicon macOS app into build/macos/TelaViva OLED.app.
Run from any directory. The first run downloads the pinned ONNX Runtime.
EOF
}

if [ "${1:-}" = "--help" ]; then
  usage
  exit 0
elif [ "$#" -ne 0 ]; then
  usage >&2
  exit 2
fi

fail() {
  printf 'build-macos: %s\n' "$1" >&2
  exit 1
}

[ "$(uname -s)" = "Darwin" ] || fail "This build requires macOS."
[ "$(uname -m)" = "arm64" ] || fail "This build targets Apple Silicon (arm64); Intel Macs are deferred."

for tool in rustup cargo python3 xcrun; do
  command -v "$tool" >/dev/null 2>&1 || fail "Missing $tool. Install Rust via rustup, Python 3, and Xcode Command Line Tools."
done

cd "$ROOT"
if ! xcrun --sdk macosx --show-sdk-path >/dev/null; then
  fail "macOS SDK unavailable. Install or select Xcode Command Line Tools."
fi
rustup target add aarch64-apple-darwin

RUNTIME="$ROOT/tools/inference/runtime/macos-arm64/libonnxruntime.dylib"
if ! "$ROOT/tools/inference/fetch-macos-runtime.sh" --check; then
  for tool in curl shasum tar; do
    command -v "$tool" >/dev/null 2>&1 || fail "Missing $tool, needed to fetch the pinned ONNX Runtime."
  done
  "$ROOT/tools/inference/fetch-macos-runtime.sh"
fi

cargo build --workspace --release --target aarch64-apple-darwin --locked
TARGET_DIR="$(cargo metadata --locked --no-deps --format-version 1 | python3 -c 'import json,sys; print(json.load(sys.stdin)["target_directory"])')"
TELAVIVA_CORE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_core.a" \
TELAVIVA_INFERENCE_LIB="$TARGET_DIR/aarch64-apple-darwin/release/libtelaviva_inference.a" \
  "$ROOT/packaging/macos/build-app.sh"
