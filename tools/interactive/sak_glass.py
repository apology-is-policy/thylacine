#!/usr/bin/env python3
# sak_glass.py -- how much of a capture is the trusted screen's ground.
#
#   sak_glass.py RENDER.rs FILE.png
#
# Prints "ground N", the percent of pixels exactly that colour, then the three
# commonest colours of the capture. The ground is read from the renderer, so the
# check cannot go stale against it. A log line from lictor says what it asked the
# device for; this reads what reached the glass.

import re
import sys
from collections import Counter

from gfx_fp import read_png


def ground(render_rs):
    with open(render_rs) as f:
        m = re.search(r"^const BG: u32 = 0xff([0-9a-f]{6});", f.read(), re.M)
    if not m:
        sys.exit("sak_glass: no BG in " + render_rs)
    return bytes.fromhex(m.group(1))


def main():
    want = ground(sys.argv[1])
    w, h, bpp, px = read_png(sys.argv[2])
    seen = Counter(bytes(px[o:o + 3]) for o in range(0, w * h * bpp, bpp))
    print("ground", seen[want] * 100 // (w * h))
    print(" ".join("#%s:%d" % (c.hex(), n * 100 // (w * h)) for c, n in seen.most_common(3)))


if __name__ == "__main__":
    main()
