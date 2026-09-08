#!/usr/bin/env python3
# gfx_region.py -- count the pixels of a screendump PNG rect that are NOT a
# given colour. The ls-gfx-age gate's instrument (GPU-DESIGN.md 4.5.8c).
#
#   gfx_region.py FILE.png X0 Y0 X1 Y1 [R G B]
#   gfx_region.py --ink FILE.png X0 Y0 X1 Y1 BR BG BB R G B
#
# Prints "<off> <total> <dom_r> <dom_g> <dom_b>": `off` pixels in the
# half-open rect [X0,X1)x[Y0,Y1) differ from (R,G,B) (default: the Bonfire
# console background #0e0c0c, UTOPIA-VISUAL.md 1.1), out of `total`; `dom` is
# the rect's most frequent colour, for the failure message. Every pixel is
# read -- no stride -- because the gate asserts `off == 0` on the negative leg,
# and a subsampled zero would prove nothing about the pixels it skipped.
#
# `--ink` prints ONE number: how many pixels of the rect lie ON the segment
# from the ground (BR,BG,BB) to the ink (R,G,B) -- projection t >= 0.5 (at
# least half the ink) with a perpendicular residual <= 16. That is the
# witness for an ANTIALIASED ink: a glyph's pixels lie on the blend line
# from the ground to the ink, most of them far from the ink itself, so a
# box tolerance around the ink misses the body and, widened enough to catch
# it, catches the other ink's blends too (ember and cinnabar sit within 80
# of each other on every channel). On the blend line a glyph's body counts
# and the OTHER ink's blends do not (an ember blend never comes within 16
# of the cinnabar line at t >= 0.5, and vice versa: the lines only converge
# at the ground, below t = 0.5). A scenario asks for the ink it wants AND
# for zero of the ink it must not see, so the leg discriminates two states
# rather than any ink at all.
#
# A REPORTER, not a judge: the scenario owns the thresholds, next to the
# argument for them. Reuses gfx_fp.py's stdlib-only PNG decoder.

import sys
from collections import Counter

from gfx_fp import read_png


def clamp_rect(args, w, h):
    x0, y0, x1, y1 = (int(v) for v in args)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w, x1), min(h, y1)
    if x1 <= x0 or y1 <= y0:
        sys.stderr.write(f"gfx_region.py: empty rect after clamping to {w}x{h}\n")
        sys.exit(2)
    return x0, y0, x1, y1


def ink(argv):
    if len(argv) != 11:
        sys.stderr.write("usage: gfx_region.py --ink FILE.png X0 Y0 X1 Y1 BR BG BB R G B\n")
        sys.exit(2)
    w, h, bpp, px = read_png(argv[0])
    x0, y0, x1, y1 = clamp_rect(argv[1:5], w, h)
    ground = [int(v) for v in argv[5:8]]
    want = [int(v) for v in argv[8:11]]
    d = [want[i] - ground[i] for i in range(3)]
    dd = sum(v * v for v in d)
    if dd == 0:
        sys.stderr.write("gfx_region.py: the ink equals the ground\n")
        sys.exit(2)
    n = 0
    for y in range(y0, y1):
        row = y * w * bpp
        for x in range(x0, x1):
            i = row + x * bpp
            p = [px[i] - ground[0], px[i + 1] - ground[1], px[i + 2] - ground[2]]
            t = sum(p[k] * d[k] for k in range(3)) / dd
            if t < 0.5:
                continue
            resid = sum((p[k] - t * d[k]) ** 2 for k in range(3)) ** 0.5
            if resid <= 16:
                n += 1
    print(n)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--ink":
        ink(sys.argv[2:])
        return
    if len(sys.argv) not in (6, 9):
        sys.stderr.write("usage: gfx_region.py FILE.png X0 Y0 X1 Y1 [R G B]\n")
        sys.exit(2)
    w, h, bpp, px = read_png(sys.argv[1])
    x0, y0, x1, y1 = clamp_rect(sys.argv[2:6], w, h)
    want = tuple(int(v) for v in sys.argv[6:9]) if len(sys.argv) == 9 else (14, 12, 12)
    off = 0
    hist = Counter()
    for y in range(y0, y1):
        row = y * w * bpp
        for x in range(x0, x1):
            i = row + x * bpp
            c = (px[i], px[i + 1], px[i + 2])
            hist[c] += 1
            if c != want:
                off += 1
    total = (x1 - x0) * (y1 - y0)
    dom = hist.most_common(1)[0][0]
    print(f"{off} {total} {dom[0]} {dom[1]} {dom[2]}")


if __name__ == "__main__":
    main()
