#!/usr/bin/env python3
# gfx_frame.py -- find the closed hairline frames in a screendump PNG: four
# sides of one colour that is not the ground. The ls-halcyon-lantern gate's
# instrument for a Beacon aside (HALCYON-VISUAL.md 8.4: a hairline on four
# sides, no ground, the passage laid inside).
#
#   gfx_frame.py FILE.png
#
# Prints one line per frame, then "frames N":
#
#   X0 Y0 X1 Y1 INK FILLED LEFT TOP RIGHT BOTTOM #RRGGBB
#
# X0..Y1 are the sides' own columns and rows, inclusive. INK counts the pixels
# strictly inside that are not the ground, the commonest colour of the whole
# capture. FILLED is 1 when the inside's commonest colour is not the ground: a
# code block has a ground of its own, an aside must not. LEFT..BOTTOM are the
# empty columns and rows between each side and the nearest ink inside, the
# padding as painted (-1 when nothing is inside). The colour is the sides'.
#
# A long side is a run of at least MIN_SIDE pixels of one colour, so a glyph's
# stroke is never one; two long sides with the same columns are a frame only
# when both short sides join them unbroken, and only when every side is at most
# THICK pixels deep at its middle -- a filled bar's rows pair with each other
# and join at its edges, and it is not a frame. A thick side pairs with itself,
# so a frame inside another with the same columns and a top and bottom within
# THICK rows of the outer's is dropped. A REPORTER, not a judge: the scenario
# owns the thresholds, next to the argument for them. Reuses gfx_fp.py's
# stdlib-only PNG decoder.

import sys
from collections import Counter

from gfx_fp import read_png

MIN_SIDE = 120
MIN_TALL = 24
THICK = 3


def frames(path):
    w, h, bpp, px = read_png(path)
    stride = w * bpp

    def at(x, y):
        o = y * stride + x * bpp
        return bytes(px[o:o + 3])

    def depth(x, y, dx, dy, c):
        n = 0
        while n <= THICK and 0 <= x < w and 0 <= y < h and at(x, y) == c:
            n, x, y = n + 1, x + dx, y + dy
        return n

    ground = Counter(bytes(px[o:o + 3]) for o in range(0, len(px), bpp)).most_common(1)[0][0]

    runs = []
    for y in range(h):
        row = px[y * stride:(y + 1) * stride]
        x = 0
        while x < w:
            c = row[x * bpp:x * bpp + 3]
            e = x + 1
            while e < w and row[e * bpp:e * bpp + 3] == c:
                e += 1
            if e - x >= MIN_SIDE and bytes(c) != ground:
                runs.append((y, x, e - 1, bytes(c)))
            x = e

    found = []
    for i, (top, x0, x1, c) in enumerate(runs):
        for bottom, bx0, bx1, bc in runs[i + 1:]:
            if (bx0, bx1, bc) != (x0, x1, c) or bottom - top + 1 < MIN_TALL:
                continue
            if all(at(x0, y) == c and at(x1, y) == c for y in range(top + 1, bottom)):
                xm, ym = (x0 + x1) // 2, (top + bottom) // 2
                sides = (depth(xm, top, 0, 1, c), depth(xm, bottom, 0, -1, c),
                         depth(x0, ym, 1, 0, c), depth(x1, ym, -1, 0, c))
                if max(sides) <= THICK:
                    found.append((x0, top, x1, bottom, c))

    found.sort(key=lambda f: (f[2] - f[0]) * (f[3] - f[1]), reverse=True)
    kept = []
    for f in found:
        if not any(
            k[0] == f[0] and k[2] == f[2] and k[4] == f[4]
            and k[1] <= f[1] <= k[1] + THICK and k[3] - THICK <= f[3] <= k[3]
            for k in kept
        ):
            kept.append(f)

    out = []
    for x0, y0, x1, y1, c in sorted(kept, key=lambda f: (f[1], f[0])):
        inside = Counter()
        lo_x, lo_y, hi_x, hi_y = w, h, -1, -1
        for y in range(y0 + 1, y1):
            for x in range(x0 + 1, x1):
                p = at(x, y)
                inside[p] += 1
                if p != ground:
                    lo_x, hi_x = min(lo_x, x), max(hi_x, x)
                    lo_y, hi_y = min(lo_y, y), max(hi_y, y)
        ink = sum(inside.values()) - inside[ground]
        filled = int(bool(inside) and inside.most_common(1)[0][0] != ground)
        if hi_x < 0:
            pad = (-1, -1, -1, -1)
        else:
            pad = (lo_x - x0 - 1, lo_y - y0 - 1, x1 - hi_x - 1, y1 - hi_y - 1)
        out.append((x0, y0, x1, y1, ink, filled) + pad + ("#%02x%02x%02x" % tuple(c),))
    return out


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: gfx_frame.py FILE.png")
    found = frames(sys.argv[1])
    for f in found:
        print(" ".join(str(v) for v in f))
    print("frames %d" % len(found))
