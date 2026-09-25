#!/usr/bin/env python3
"""Ink coverage of a screendump: how many pixels are NOT the commonest colour.

The question a capture-based gate usually wants answered is "did anything get
DRAWN", and the naive forms of it are all wrong in the same direction. File size
is compression, not content. A colour count rises just as happily for gradient
noise as for text. Sampling fixed points assumes a layout.

Counting non-ground pixels asks the honest question instead: a tile the
compositor cleared and stopped is overwhelmingly one colour, and a tile with a
rendered document is not. It pins no palette, so it survives a theme change --
which matters here, because the ground it should ignore is whatever the loaded
theme says it is, and a gate that hard-coded Daylight's would go red on any
other theme (the standing `ls-gfx-compose` defect).

Prints one integer: the non-ground pixel count.

    gfx_ink.py <png>
    gfx_ink.py <png> --rect X0 Y0 X1 Y1

`--rect` counts only inside a rectangle given as FRACTIONS of the image, so a
gate asks "is this band empty" without pinning a resolution. The ground is still
the commonest colour of the WHOLE image: a band that is all text must not get to
call its text the ground.
"""

import struct
import sys
import zlib
from collections import Counter

_BPP = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _unfilter(raw, width, height, bpp):
    """Undo the PNG per-scanline filters, yielding each finished scanline."""
    stride = width * bpp
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + stride])
        pos += stride
        if ftype == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xFF
        elif ftype == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ftype == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        elif ftype != 0:
            raise SystemExit("gfx_ink: unknown PNG filter type %d" % ftype)
        yield line
        prev = line


def ink(path, rect=None):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("gfx_ink: %s is not a PNG" % path)
    width = height = color = None
    idat = bytearray()
    i = 8
    while i + 8 <= len(data):
        (length,) = struct.unpack(">I", data[i : i + 4])
        ctype = data[i + 4 : i + 8]
        body = data[i + 8 : i + 8 + length]
        if ctype == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8:
                raise SystemExit("gfx_ink: only 8-bit channels are handled")
            if color not in _BPP:
                raise SystemExit("gfx_ink: unhandled colour type %d" % color)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        i += 12 + length
    if width is None:
        raise SystemExit("gfx_ink: no IHDR")

    bpp = _BPP[color]
    keep = min(3, bpp)  # RGB, or the grey byte; alpha is not a colour
    x0, y0, x1, y1 = 0, 0, width, height
    if rect is not None:
        x0, y0 = int(rect[0] * width), int(rect[1] * height)
        x1, y1 = int(rect[2] * width), int(rect[3] * height)
    counts = Counter()
    inside = Counter()
    raw = zlib.decompress(bytes(idat))
    for y, line in enumerate(_unfilter(raw, width, height, bpp)):
        for x in range(0, width * bpp, bpp):
            px = bytes(line[x : x + keep])
            counts[px] += 1
            if y0 <= y < y1 and x0 <= x // bpp < x1:
                inside[px] += 1
    if not counts:
        return 0
    ground = counts.most_common(1)[0][0]
    return sum(inside.values()) - inside[ground]


if __name__ == "__main__":
    if len(sys.argv) == 2:
        print(ink(sys.argv[1]))
    elif len(sys.argv) == 7 and sys.argv[2] == "--rect":
        r = [float(v) for v in sys.argv[3:7]]
        if not (0 <= r[0] < r[2] <= 1 and 0 <= r[1] < r[3] <= 1):
            raise SystemExit("gfx_ink: --rect wants 0 <= X0 < X1 <= 1, 0 <= Y0 < Y1 <= 1")
        print(ink(sys.argv[1], r))
    else:
        raise SystemExit(__doc__)
