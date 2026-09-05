#!/usr/bin/env python3
# gfx_region.py -- count the pixels of a screendump PNG rect that are NOT a
# given colour. The ls-gfx-age gate's instrument (GPU-DESIGN.md 4.5.8c).
#
#   gfx_region.py FILE.png X0 Y0 X1 Y1 [R G B]
#
# Prints "<off> <total> <dom_r> <dom_g> <dom_b>": `off` pixels in the
# half-open rect [X0,X1)x[Y0,Y1) differ from (R,G,B) (default: the Bonfire
# console background #0e0c0c, UTOPIA-VISUAL.md 1.1), out of `total`; `dom` is
# the rect's most frequent colour, for the failure message. Every pixel is
# read -- no stride -- because the gate asserts `off == 0` on the negative leg,
# and a subsampled zero would prove nothing about the pixels it skipped.
#
# A REPORTER, not a judge: the scenario owns the thresholds, next to the
# argument for them. Reuses gfx_fp.py's stdlib-only PNG decoder.

import sys
from collections import Counter

from gfx_fp import read_png


def main():
    if len(sys.argv) not in (6, 9):
        sys.stderr.write("usage: gfx_region.py FILE.png X0 Y0 X1 Y1 [R G B]\n")
        sys.exit(2)
    w, h, bpp, px = read_png(sys.argv[1])
    x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
    want = tuple(int(v) for v in sys.argv[6:9]) if len(sys.argv) == 9 else (14, 12, 12)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w, x1), min(h, y1)
    if x1 <= x0 or y1 <= y0:
        sys.stderr.write(f"gfx_region.py: empty rect after clamping to {w}x{h}\n")
        sys.exit(2)
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
