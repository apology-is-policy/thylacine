#!/usr/bin/env python3
# gfx_fp.py -- fingerprint a screendump PNG for the ls-gfx-quake gate.
#
# Prints "<rolling-hash> <distinct-color-buckets>":
#   - the hash lets the scenario assert two frames DIFFER (frames advancing
#     through the blocking present), without pixel-exact golden images.
#   - the color-bucket count (RGB quantized to 32-level bins) distinguishes
#     Quake's textured 3D world (many buckets) from a flat/black surface.
#
# Decodes RGB/RGBA 8-bit non-interlaced PNGs with stdlib only (no PIL) --
# the tools/screendump.sh QMP output format.

import sys
import zlib
import struct


def read_png(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, w, h, ct, idat = 8, 0, 0, 0, b""
    while pos < len(d):
        ln, typ = struct.unpack(">I4s", d[pos:pos + 8])
        pos += 8
        chunk = d[pos:pos + ln]
        pos += ln + 4
        if typ == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
    raw = zlib.decompress(idat)
    bpp = {2: 3, 6: 4}[ct]
    stride = w * bpp
    out = bytearray(w * h * bpp)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, out


def main():
    w, h, bpp, px = read_png(sys.argv[1])
    acc = 0
    buckets = set()
    for i in range(0, w * h * bpp, bpp * 211):
        r, g, b = px[i], px[i + 1], px[i + 2]
        acc = (acc * 131 + r * 7 + g * 13 + b * 17) & 0xFFFFFFFF
        buckets.add((r // 32, g // 32, b // 32))
    print(f"{acc} {len(buckets)}")


if __name__ == "__main__":
    main()
