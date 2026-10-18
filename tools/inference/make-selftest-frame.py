#!/usr/bin/env python3
"""Build the macOS TVSF self-test frame from the same CC0 portrait as Windows.

Uses only Python's standard library. The 240-pixel portrait is centred in a
320x240 opaque black BGRA frame; the header matches the Windows TVSF layout.
"""

import argparse
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "crates/telaviva-inference/tests/fixtures/cc0-face.ppm"


def make_frame(source: bytes) -> bytes:
    position = 0

    def token() -> bytes:
        nonlocal position
        while position < len(source):
            if source[position:position + 1] == b"#":
                newline = source.find(b"\n", position)
                position = len(source) if newline < 0 else newline + 1
            elif source[position] in b" \t\r\n":
                position += 1
            else:
                break
        start = position
        while position < len(source) and source[position] not in b" \t\r\n":
            position += 1
        return source[start:position]

    if token() != b"P6":
        raise ValueError("Expected binary P6 fixture")
    width, height, maximum = int(token()), int(token()), int(token())
    if width != height or width <= 0 or maximum != 255:
        raise ValueError("Expected a square 8-bit RGB portrait")
    if position >= len(source) or source[position] not in b" \t\r\n":
        raise ValueError("Missing PPM pixel separator")
    position += 1
    rgb = source[position:]
    if len(rgb) != width * height * 3:
        raise ValueError("Unexpected fixture pixel length")
    # Deterministic nearest-neighbour scaling keeps this build step dependency-free.
    pixels = bytearray(b"\x00\x00\x00\xff" * (320 * 240))
    for y in range(240):
        for x in range(240):
            source_at = ((y * height // 240) * width + x * width // 240) * 3
            target_at = (y * 320 + x + 40) * 4
            pixels[target_at:target_at + 3] = rgb[source_at:source_at + 3][::-1]
    return b"TVSF" + struct.pack("<III", 320, 240, 1280) + pixels


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    frame = make_frame(FIXTURE.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(frame)
    print(f"Wrote self-test frame: {args.output} ({len(frame)} bytes)")
