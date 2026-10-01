#!/usr/bin/env python3
# make-deck-picture.py -- generate the demo deck's picture slide (LANTERN-DESIGN
# 5.1 and 14): deck/04-lantern.png.
#
# A deterministic 640x400 RGB PNG (color type 2, 8-bit, no interlace) using only
# the Python stdlib (zlib). Committed alongside its output so the bake needs no
# build-time Python; regenerate with `python3 make-deck-picture.py` from this
# directory if the picture ever changes. It lives OUTSIDE deck/ because the
# build installs every file in deck/ into /deck.
#
# The picture: a magic lantern's projected disc, warm amber, crossed by the
# thylacine's dark stripes, on a near-black ground with a faint halo. The amber
# is saturated well past anything Halcyon's own palette draws in bulk, which is
# what lets tools/interactive/ls-halcyon-lantern.exp find the picture in a
# capture and not find it on the text slide one keystroke away.

import math
import struct
import zlib

W, H = 640, 400
CX, CY, R = 320.0, 200.0, 170.0
GROUND = (14, 17, 22)
HOT = (242, 160, 48)    # the lamp's hot spot
RIM = (196, 102, 28)    # the disc's edge
STRIPES = 9
STRIPE_GAP = 34.0


def lerp(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def in_stripe(x, y):
    top = CY - R
    t = (y - top) / (2 * R)          # 0 at the disc's top, 1 at its bottom
    half = 7.0 - 3.0 * t             # a stripe narrows down the flank
    bend = 9.0 * math.sin(math.pi * t)
    for k in range(STRIPES):
        sx = CX + (k - (STRIPES - 1) / 2) * STRIPE_GAP + bend
        if abs(x - sx) <= half:
            return True
    return False


def pixel(x, y):
    px, py = x + 0.5, y + 0.5
    r = math.hypot(px - CX, py - CY)
    if r >= R + 0.75:
        glow = max(0.0, 1.0 - (r - R) / 60.0) * 0.10
        c = lerp(GROUND, HOT, glow)
    else:
        t = min(r / R, 1.0)
        c = lerp(HOT, RIM, t * t)
        if in_stripe(px, py):
            c = tuple(v * 0.35 for v in c)
        cover = min(1.0, (R + 0.75 - r) / 1.5)
        c = lerp(GROUND, c, cover)
    return tuple(int(round(v)) for v in c)


def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def main():
    rows = []
    prev = bytes(W * 3)
    for y in range(H):
        cur = bytes(v for x in range(W) for v in pixel(x, y))
        # Filter type 2 (Up): the disc changes slowly down a column, so the
        # residuals are mostly zero and the file stays small.
        rows.append(b"\x02" + bytes((c - p) & 0xFF for c, p in zip(cur, prev)))
        prev = cur
    ihdr = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    with open("deck/04-lantern.png", "wb") as f:
        f.write(png)
    print("deck/04-lantern.png: %dx%d, %d bytes" % (W, H, len(png)))


if __name__ == "__main__":
    main()
