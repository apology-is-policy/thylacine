#!/usr/bin/env python3
"""Explain a magnified glyph capture against the renderer's own raster.

    capture-explained.py <capture.png> <renderer-glyph.png> <out.png>

The capture is a screenshot of a magnified glyph (a Color Meter view, a
zoomed screenshot); the renderer glyph is the same glyph at device pixels
(e.g. a `wk` snapshot crop). The script (1) finds the capture's block grid
from the flat runs of its profiles, (2) samples one value per block, and
(3) tests three observation models against the renderer's raster -- the
raster itself, a 2x2 box average of it, and a DECIMATION of it (every other
device pixel, four parities) -- reporting the RMS ink error of each at its
best integer alignment, then writes a composite: capture blocks | the best
model | the full raster. Stdlib only.
"""
import sys
from pngio import read_png, write_png, gray, ink


def profiles_period(N):
    """Block period + phase from the flat runs of the middle row/column."""
    h = len(N)
    w = len(N[0])
    y = h // 2
    row = [int(v * 9.99) for v in N[y]]
    # transitions
    tx = [x for x in range(1, w) if row[x] != row[x - 1]]
    gaps = [b - a for a, b in zip(tx, tx[1:]) if b - a >= 4]
    period = max(set(gaps), key=gaps.count) if gaps else 16
    phase = tx[0] % period if tx else 0
    return period, phase


def block_target(N, period, phase):
    h = len(N)
    w = len(N[0])
    xs = list(range(phase + period // 2, w, period))
    ys = list(range(phase + period // 2, h, period))
    return [[N[y][x] for x in xs] for y in ys]


def best_align(A, B):
    ah, aw = len(A), len(A[0])
    bh, bw = len(B), len(B[0])
    best = None
    for dy in range(-ah, bh):
        for dx in range(-aw, bw):
            se = 0.0
            n = 0
            for j in range(bh):
                for i in range(bw):
                    y = j - dy
                    x = i - dx
                    a = A[y][x] if 0 <= y < ah and 0 <= x < aw else 0.0
                    se += (a - B[j][i]) ** 2
                    n += 1
            rms = (se / n) ** 0.5
            if best is None or rms < best[0]:
                best = (rms, dx, dy)
    return best


def box2(T, ox, oy):
    H, W = len(T), len(T[0])
    return [[(T[y][x] + T[y][x + 1] + T[y + 1][x] + T[y + 1][x + 1]) / 4 for x in range(ox, W - 1, 2)] for y in range(oy, H - 1, 2)]


def decimate(T, ox, oy):
    return [[T[y][x] for x in range(ox, len(T[0]), 2)] for y in range(oy, len(T), 2)]


def shade(v):
    return bytes([int(242 - (242 - 26) * v), int(235 - (235 - 18) * v), int(224 - (224 - 10) * v)])


def panel(M, cell):
    H, W = len(M), len(M[0])
    out = bytearray()
    for j in range(H):
        for _ in range(cell):
            for i in range(W):
                out += shade(M[j][i]) * cell
    return W * cell, H * cell, out


def main():
    cap, ref, out = sys.argv[1:4]
    nw, nh, ng = gray(cap)
    bg = max(map(max, ng))
    fg = min(map(min, ng))
    N = ink(ng, float(bg), float(fg))
    period, phase = profiles_period(N)
    B = block_target(N, period, phase)
    print(f"capture: {nw}x{nh}, block period {period} px, phase {phase}, ground {bg}, darkest {fg} -> {len(B[0])}x{len(B)} blocks")
    w, h, g = gray(ref)
    T = ink(g, float(max(map(max, g))), float(min(map(min, g))))
    models = [("raw raster", T)]
    for ox in (0, 1):
        for oy in (0, 1):
            models.append((f"2x2 box parity ({ox},{oy})", box2(T, ox, oy)))
            models.append((f"decimated parity ({ox},{oy})", decimate(T, ox, oy)))
    results = []
    for name, M in models:
        rms, dx, dy = best_align(M, B)
        results.append((rms, name, M))
        print(f"  {name:<24} rms {rms:.4f}")
    results.sort(key=lambda r: r[0])
    rms, name, M = results[0]
    print(f"best: {name} (rms {rms:.4f})")
    cell = max(4, 240 // max(len(B), len(B[0])))
    panels = [panel(B, cell), panel(M, cell), panel(T, max(2, cell // 2))]
    Wt = sum(p[0] for p in panels) + 12
    Ht = max(p[1] for p in panels)
    rgb = bytearray(bytes([0xA8, 0x98, 0x80]) * (Wt * Ht))
    x = 0
    for (pw, ph, pd) in panels:
        for j in range(ph):
            rgb[(j * Wt + x) * 3:(j * Wt + x + pw) * 3] = pd[j * pw * 3:(j + 1) * pw * 3]
        x += pw + 6
    write_png(out, Wt, Ht, rgb)
    print(f"wrote {out}: capture blocks | {name} | full raster")


if __name__ == "__main__":
    main()
