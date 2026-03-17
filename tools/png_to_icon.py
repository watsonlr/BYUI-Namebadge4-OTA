#!/usr/bin/env python3
"""
png_to_icon.py — Convert a PNG to a raw 308×72 RGB565 binary for the BYUI eBadge.

The output is a flat binary of 308×72×2 = 44 352 bytes, each pixel stored
big-endian RGB565 (high byte first) — the wire format expected by the ILI9341
and consumed directly by display_draw_bitmap() on the badge.

Each icon is blitted centred inside a 76 px tall × 320 px wide tile slot,
leaving a 6 px coloured border left/right and 2 px top/bottom that shows the
selection highlight colour when the user scrolls through the app menu.

Usage
-----
    python tools/png_to_icon.py <input.png> <output.bin>

    input.png   — source image (any size; resized to 308×72 with LANCZOS)
    output.bin  — destination raw binary

Host the .bin files on GitHub (raw.githubusercontent.com) and reference them
in the app manifest as:

    { "icon": "https://raw.githubusercontent.com/.../icons/my_app_icon.bin", ... }

Requirements
------------
    pip install Pillow
"""

import sys
import os
from PIL import Image

ICON_W = 308
ICON_H = 72


def rgb888_to_rgb565_be(r: int, g: int, b: int) -> int:
    """Return big-endian (wire-order) RGB565 as a 16-bit value."""
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    # Byte-swap to big-endian (high byte first) so the C array can be sent
    # straight to the ILI9341 SPI bus without runtime conversion.
    return ((rgb565 & 0x00FF) << 8) | ((rgb565 & 0xFF00) >> 8)


def convert(src: str, dst: str) -> None:
    img = Image.open(src).convert("RGB")

    if img.size != (ICON_W, ICON_H):
        print(f"Resizing {img.size[0]}×{img.size[1]} → {ICON_W}×{ICON_H}")
        img = img.resize((ICON_W, ICON_H), Image.LANCZOS)
    else:
        print(f"Image is already {ICON_W}×{ICON_H} — no resize needed.")

    data = bytearray()
    for y in range(ICON_H):
        for x in range(ICON_W):
            r, g, b = img.getpixel((x, y))
            v = rgb888_to_rgb565_be(r, g, b)
            data.append((v >> 8) & 0xFF)
            data.append(v & 0xFF)

    with open(dst, "wb") as f:
        f.write(data)

    size = ICON_W * ICON_H * 2
    print(f"Written: {dst}  ({size} bytes, {ICON_W}×{ICON_H} big-endian RGB565)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: python {os.path.basename(sys.argv[0])} <input.png> <output.bin>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
