#!/usr/bin/env python3
"""Copy verified inference assets into a macOS app bundle's resources/frameworks."""

from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = {
    ROOT / "assets/model/face_detection_yunet_2023mar.onnx": (
        "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4",
        "model",
    ),
    ROOT / "tools/inference/runtime/macos-arm64/libonnxruntime.dylib": (
        "530cdb5a0de774677d369a83ec8912b0242e9769acd12d34b61412ee6ae368ae",
        "runtime",
    ),
}


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources-dir", type=Path, required=True, help="App bundle Resources directory")
    parser.add_argument("--frameworks-dir", type=Path, required=True, help="App bundle Frameworks directory")
    args = parser.parse_args()

    destinations = {
        "model": args.resources_dir / "models/face_detection_yunet_2023mar.onnx",
        "runtime": args.frameworks_dir / "libonnxruntime.dylib",
    }
    for source, (expected, role) in ASSETS.items():
        actual = digest(source)
        if actual != expected:
            raise SystemExit(f"Refusing to package {role}: SHA-256 mismatch for {source}")
        destination = destinations[role]
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        print(f"Packaged {role}: {destination}")


if __name__ == "__main__":
    main()
