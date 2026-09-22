#!/usr/bin/env python3
"""Count strongly coloured pixels; excludes Halcyon's neutral/amber typography."""
import sys
from gfx_fp import read_png
w, h, bpp, pixels = read_png(sys.argv[1])
count = 0
for i in range(0, len(pixels), bpp):
    r, g, b = pixels[i:i + 3]
    if max(r, g, b) - min(r, g, b) >= 90 and max(r, g, b) >= 130:
        count += 1
print(count)
