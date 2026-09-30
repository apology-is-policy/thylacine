#!/usr/bin/env python3
"""Count the lantern demo deck's amber in a screendump PNG.

A pixel counts when red > 200, 120 <= green < 190 and blue < 60: the predicate
lantern's host test the_shipped_picture_is_one_view_shows pins against
usr/lantern/deck/04-lantern.png. Blue under 60 leaves out Halcyon's ember
accent (0xE07840) and its blends with either theme's ground.

    gfx_amber.py <png>
"""
import sys
from gfx_fp import read_png

w, h, bpp, pixels = read_png(sys.argv[1])
count = 0
for i in range(0, len(pixels), bpp):
    r, g, b = pixels[i:i + 3]
    if r > 200 and 120 <= g < 190 and b < 60:
        count += 1
print(count)
