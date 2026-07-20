#!/usr/bin/env python3
# tools/ppm-sample.py -- print the RGB triple at (X, Y) of a P6 PPM as
# "R G B" (G-6; the ls-gfx-panes battery's pixel asserts sample the pane
# centers tapestry-battery prints, against a `screendump.sh -P` capture).
#
#   tools/ppm-sample.py FILE.ppm X Y
import sys


def parse_ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise RuntimeError(f"{path}: not a P6 PPM")
    toks, i, n = [], 2, len(data)
    while len(toks) < 3 and i < n:
        c = data[i:i + 1]
        if c in b" \t\r\n":
            i += 1
        elif c == b"#":
            while i < n and data[i:i + 1] != b"\n":
                i += 1
        else:
            j = i
            while j < n and data[j:j + 1] not in b" \t\r\n":
                j += 1
            toks.append(int(data[i:j]))
            i = j
    i += 1
    w, h, maxval = toks
    if maxval != 255:
        raise RuntimeError(f"{path}: maxval {maxval} != 255")
    px = data[i:]
    if len(px) < w * h * 3:
        raise RuntimeError(f"{path}: truncated pixel data")
    return w, h, px


def main():
    if len(sys.argv) != 4:
        print("usage: ppm-sample.py FILE.ppm X Y", file=sys.stderr)
        return 2
    path, x, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    w, h, px = parse_ppm(path)
    if not (0 <= x < w and 0 <= y < h):
        print(f"ppm-sample: ({x},{y}) outside {w}x{h}", file=sys.stderr)
        return 1
    off = (y * w + x) * 3
    print(f"{px[off]} {px[off + 1]} {px[off + 2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
