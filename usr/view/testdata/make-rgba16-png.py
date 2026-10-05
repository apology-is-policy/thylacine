#!/usr/bin/env python3
# make-rgba16-png.py -- the 16-bit decode fixture (view's lib test
# decode_png_takes_the_top_byte_of_a_16_bit_sample).
#
# A 2x2 RGBA PNG (color type 6, 16 bits a sample, no interlace) from the Python
# stdlib alone. Each sample's low byte differs from its high byte, so a decoder
# that narrowed to the wrong byte shows a different colour. Committed with its
# output; regenerate with `python3 make-rgba16-png.py` from this directory.

import struct
import zlib

PIXELS = [
    [(0xE0FF, 0x2001, 0x20FE, 0xFFFF), (0x20FF, 0xE000, 0x2080, 0xFFFF)],
    [(0x1234, 0x5678, 0x9ABC, 0x80FF), (0x0000, 0x0000, 0x0000, 0x0000)],
]


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def main():
    h, w = len(PIXELS), len(PIXELS[0])
    raw = b"".join(b"\x00" + b"".join(struct.pack(">4H", *px) for px in row) for row in PIXELS)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open("../src/testdata/rgba16.png", "wb") as f:
        f.write(png)


if __name__ == "__main__":
    main()
