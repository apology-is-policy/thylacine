#!/usr/bin/env python3
# tools/ppm-colorfrac.py -- exact-color pixel-fraction check over a P6 PPM
# (a tools/screendump.sh -P capture). The ls-gfx-osd E2E asserts the OSD's
# EGA signature colors appear (panel open) and vanish (panel closed); exact
# match is sound because the virtio-gpu scanout carries the composed pixels
# verbatim (the same property screendump's -c console verify rests on).
#
# usage: ppm-colorfrac.py FILE.ppm RRGGBB MIN [MAX]
#   exit 0 iff MIN <= fraction-of-exact-RRGGBB-pixels <= MAX (MAX default 1).
#   The measured fraction prints either way (the diagnostic on failure).
import sys


def main():
    if len(sys.argv) < 4:
        print("usage: ppm-colorfrac.py FILE.ppm RRGGBB MIN [MAX]", file=sys.stderr)
        return 2
    path, want = sys.argv[1], int(sys.argv[2], 16)
    lo, hi = float(sys.argv[3]), float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
    with open(path, "rb") as f:
        data = f.read()
    # P6 header: magic, width, height, maxval -- whitespace/comment tolerant.
    toks, i = [], 0
    while len(toks) < 4:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        toks.append(data[i:j])
        i = j
    i += 1  # the single whitespace after maxval
    if toks[0] != b"P6" or int(toks[3]) != 255:
        print(f"not a maxval-255 P6 PPM: {toks[:1]}", file=sys.stderr)
        return 2
    w, h = int(toks[1]), int(toks[2])
    px = data[i : i + w * h * 3]
    if len(px) != w * h * 3:
        print("truncated pixel data", file=sys.stderr)
        return 2
    key = bytes(((want >> 16) & 255, (want >> 8) & 255, want & 255))
    n = sum(1 for o in range(0, len(px), 3) if px[o : o + 3] == key)
    frac = n / (w * h)
    ok = lo <= frac <= hi
    print(f"colorfrac {sys.argv[2]}: {frac:.4f} ({n}/{w*h}) "
          f"{'within' if ok else 'OUTSIDE'} [{lo}, {hi}]")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
