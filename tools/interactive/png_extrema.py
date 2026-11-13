#!/usr/bin/env python3
"""Per-channel (min, max) of a box of an 8-bit RGB PNG, printed the way a Python tuple prints.
Usage: png_extrema.py FILE LEFT UPPER RIGHT LOWER   (RIGHT and LOWER exclusive)"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_fp import read_png  # noqa: E402


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    left, upper, right, lower = (int(v) for v in sys.argv[2:6])
    w, h, bpp, px = read_png(sys.argv[1])
    if bpp != 3 or not (0 <= left < right <= w and 0 <= upper < lower <= h):
        sys.exit('png_extrema: %s is not an RGB PNG holding the box' % sys.argv[1])
    out = []
    for c in range(3):
        least, most = 255, 0
        for y in range(upper, lower):
            row = px[(y * w + left) * 3 + c:(y * w + right) * 3:3]
            least, most = min(least, min(row)), max(most, max(row))
        out.append((least, most))
    print(tuple(out))


if __name__ == '__main__':
    main()
