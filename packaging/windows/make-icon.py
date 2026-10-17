#!/usr/bin/env python3
"""Makes aiedit.ico, the icon of aiedit.exe, from packaging/icons/aiedit.svg.

Each size is drawn from the SVG by rsvg-convert (librsvg) and stored as PNG, which Windows
reads in icons since Vista. Run it again after changing the SVG:
    packaging/windows/make-icon.py
"""
import pathlib
import struct
import subprocess

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

here = pathlib.Path(__file__).resolve().parent
svg = here.parent / "icons" / "aiedit.svg"
images = [subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), str(svg)],
                         check=True, capture_output=True).stdout for size in SIZES]

# ICONDIR, then one ICONDIRENTRY per image, then the images.
header = struct.pack("<HHH", 0, 1, len(images))
offset = len(header) + 16 * len(images)
entries = b""
for size, png in zip(SIZES, images):
    side = 0 if size == 256 else size  # 0 stands for 256
    entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(png), offset)
    offset += len(png)
(here / "aiedit.ico").write_bytes(header + entries + b"".join(images))
print(f"aiedit.ico: {len(images)} sizes, {offset} bytes")
