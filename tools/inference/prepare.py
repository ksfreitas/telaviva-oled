#!/usr/bin/env python3
"""Copy verified inference assets into a macOS app bundle's resources/frameworks."""

from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = {
    ROOT / "assets/model/face_detection_retinaface_mb025.onnx": (
        "f4c83825f41dba8e17b522377f77cb7e2b69ec4df75168d4cb1b18dcaf03b54c",
        "model",
    ),
    ROOT / "tools/inference/runtime/macos-arm64/libonnxruntime.dylib": (
        "d3859aecdb70ea099f5b5f4185fe16f0527c6680b18731e6e96fc971ec767cca",
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
        "model": args.resources_dir / "models/face_detection_retinaface_mb025.onnx",
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
