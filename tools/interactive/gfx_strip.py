#!/usr/bin/env python3
# gfx_strip.py -- vertical-orientation evidence for the Warp-4c quake gate:
# mean RGB of the top and bottom h/10 strips of a screendump PNG.
#
# Prints "top <r> <g> <b> bottom <r> <g> <b>" (integers). The caller owns
# the verdict: GLQuake draws its status bar across the BOTTOM of the frame
# and sky/world at the top, so a flipped direct scanout swaps the two
# signatures. Kept as a REPORTER so the thresholds live in the scenario
# next to the calibration notes, not buried here.
#
# Reuses gfx_fp.py's stdlib-only PNG decoder (same directory).

import sys

from gfx_fp import read_png


def strip_mean(px, w, bpp, y0, y1):
    r = g = b = n = 0
    for y in range(y0, y1):
        row = y * w * bpp
        for x in range(0, w, 7):
            i = row + x * bpp
            r += px[i]
            g += px[i + 1]
            b += px[i + 2]
            n += 1
    return (r // n, g // n, b // n) if n else (0, 0, 0)


def main():
    w, h, bpp, px = read_png(sys.argv[1])
    s = max(1, h // 10)
    tr, tg, tb = strip_mean(px, w, bpp, 0, s)
    br, bg, bb = strip_mean(px, w, bpp, h - s, h)
    print(f"top {tr} {tg} {tb} bottom {br} {bg} {bb}")


if __name__ == "__main__":
    main()
