#!/usr/bin/env python3
# make-test-png.py -- generate the inline-media E2E witness card (I-47).
#
# A deterministic 640x400 RGB PNG (color type 2, 8-bit, no interlace) using only
# the Python stdlib (zlib). Committed alongside the output so the bake needs no
# build-time Python; regenerate with `python3 make-test-png.py` if the card ever
# changes. The pattern is chosen so a screenshot unambiguously shows a DECODED
# image, never garbage: six saturated color bars across the top two-thirds, a
# black->white luminance gradient below them, and a bright diagonal so a
# transposed/mis-strided decode is obvious.

import struct
import zlib

W, H = 640, 400
BARS = [
    (0xE0, 0x20, 0x20),  # red
    (0x20, 0xE0, 0x20),  # green
    (0x20, 0x20, 0xE0),  # blue
    (0xE0, 0xE0, 0x20),  # yellow
    (0x20, 0xE0, 0xE0),  # cyan
    (0xE0, 0x20, 0xE0),  # magenta
]
BAR_H = (H * 2) // 3  # color bars occupy the top two-thirds


def pixel(x, y):
    if y < BAR_H:
        r, g, b = BARS[(x * len(BARS)) // W]
    else:
        # Luminance ramp left->right in the bottom third.
        v = (x * 255) // (W - 1)
        r = g = b = v
    # A bright diagonal stripe across the whole card (strides must be right).
    if abs((x * H) // W - y) < 4:
        r, g, b = 0xFF, 0xFF, 0xFF
    return r, g, b


def main():
    raw = bytearray()
    for y in range(H):
        raw.append(0)  # filter type 0 (None) per scanline
        for x in range(W):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        out += struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        return out

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)  # 8-bit, RGB
    idat = zlib.compress(bytes(raw), 9)
    png = sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")

    with open("test.png", "wb") as f:
        f.write(png)
    print("wrote test.png: {}x{} RGB, {} bytes".format(W, H, len(png)))


if __name__ == "__main__":
    main()
