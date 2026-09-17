#!/usr/bin/env python3
# gfx_shift.py -- the lateral-shift signature between two screendumps: the
# mouse-look witness for the DOSBox gates.
#
# Prints "<shift_px> <confidence>": the horizontal shift (source pixels; the
# sign says which way the CONTENT moved on screen) that best aligns frame B to
# frame A, and how decisively it beats "no shift" (0 = not at all, 1 = fully).
#
# WHY NOT A HASH: a frame hash proves two frames DIFFER, and a live 3D game
# differs frame to frame on its own (animation, a muzzle flash, an enemy) --
# so "the frame changed after the mouse moved" proves nothing. A camera YAW is
# different in kind: it shifts the WHOLE viewport sideways. So: reduce each
# frame to a per-column mean-luma profile over the viewport band, then find the
# shift that minimizes the mean absolute difference between the two profiles.
# Animation changes brightness locally without moving the profile (best shift
# ~0); a turn moves it wholesale (best shift far from 0). The gate pairs this
# with a NO-INPUT control one variable away, so the instrument is proven on
# the same frames it judges.
#
# WHERE TO LOOK is measured, not assumed. The DOSBox surface's position inside
# its tile depends on the compositor's placement (centred on one day's frames,
# top-aligned on the next -- the first version of this tool assumed centred and
# went blind: its band covered the HUD and the black below the surface, both
# identical between frames, which dragged the optimum to 0). So by default the
# tool finds the surface itself: within the column window (the right half of
# the display by default -- the DOSBox tile in the two-tile ls-gfx layout; the
# console pane on the left is static and would pull the optimum to 0), the
# rows whose luma VARIES across the window (inset past the tile's frame lines,
# which are uniform but bright) are the surface (the empty tile ground is a
# uniform dark grey, so a brightness floor cannot separate it from a dark sky); the band is then rows 10%..80% of THAT extent (below DOSBox-X's
# menu bar, above the game's HUD).
# --extent prints the detected surface box (x0 y0 x1 y1, display pixels) so a
# gate can aim its capture click at the surface's centre. --y0/--y1 (fractions
# of the display) force a fixed band instead; --x0/--x1 set the column window.

# Same stdlib-only PNG decode as gfx_fp.py (tools/screendump.sh output).

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx_fp import read_png  # noqa: E402


def load(path):
    return read_png(path)


def surface_rows(w, h, bpp, px, cx0, cx1, spread=4.0):
    # The surface = the rows whose luma VARIES across the column window. The
    # tile's empty ground is a uniform dark grey (measured (16,16,20)), the tile
    # border a uniform beige line: both have ~zero spread, while a game frame
    # (even a dark sky with a few lit windows) varies. A luma FLOOR cannot do
    # this: the ground's mean (~18) is above a dark sky's (~7).
    rows = []
    inset = 16   # past the tile's frame lines, which are uniform but bright
    for y in range(0, h, 2):
        vals = []
        for x in range(cx0 + inset, cx1 - inset, 8):
            i = (y * w + x) * bpp
            vals.append((px[i] * 3 + px[i + 1] * 6 + px[i + 2]) / 10.0)
        if not vals:
            rows.append((y, 0.0)); continue
        m = sum(vals) / len(vals)
        var = sum((v - m) ** 2 for v in vals) / len(vals)
        rows.append((y, var ** 0.5))
    lit = [y for y, sd in rows if sd > spread]
    if not lit:
        return 0, h
    return lit[0], lit[-1] + 2


def surface_cols(w, h, bpp, px, cx0, cx1, y0, y1, spread=4.0):
    cols = []
    inset = 16
    for x in range(cx0, cx1, 2):
        vals = []
        for y in range(y0 + inset, y1 - inset, 8):
            i = (y * w + x) * bpp
            vals.append((px[i] * 3 + px[i + 1] * 6 + px[i + 2]) / 10.0)
        if not vals:
            cols.append((x, 0.0)); continue
        m = sum(vals) / len(vals)
        var = sum((v - m) ** 2 for v in vals) / len(vals)
        cols.append((x, var ** 0.5))
    lit = [x for x, sd in cols if sd > spread]
    if not lit:
        return cx0, cx1
    return lit[0], lit[-1] + 2


def band_for(img, x0f, x1f, y0f, y1f):
    # returns (cx0, cx1, cy0, cy1): fixed rows when y0f/y1f given, else the
    # measured surface extent's 5%..80%.
    w, h, bpp, px = img
    cx0, cx1 = int(w * x0f), int(w * x1f)
    if y0f is not None and y1f is not None:
        return cx0, cx1, int(h * y0f), int(h * y1f)
    top, bot = surface_rows(w, h, bpp, px, cx0, cx1)
    ext = bot - top
    # 10%..80% of the extent: below DOSBox-X's menu bar, above the game's HUD.
    return cx0, cx1, top + int(ext * 0.10), top + int(ext * 0.80)


def profile(img, cx0, cx1, cy0, cy1, step):
    w, h, bpp, px = img
    cols = []
    for x in range(cx0, cx1, step):
        acc = 0
        n = 0
        for y in range(cy0, cy1, 2):
            i = (y * w + x) * bpp
            acc += px[i] * 3 + px[i + 1] * 6 + px[i + 2]   # cheap luma
            n += 1
        cols.append(acc / n if n else 0.0)
    return cols


def sad(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i] - b[i]) for i in range(n)) / n if n else 0.0


def best_shift(pa, pb, max_shift):
    # shift s: compare A[i] with B[i - s] -> a positive s means B's content sits
    # s columns to the RIGHT of where A had it.
    best_s, best_v = 0, None
    base = sad(pa, pb)
    for s in range(-max_shift, max_shift + 1):
        if s >= 0:
            v = sad(pa[s:], pb[:len(pb) - s])
        else:
            v = sad(pa[:len(pa) + s], pb[-s:])
        if best_v is None or v < best_v:
            best_s, best_v = s, v
    conf = 0.0 if base <= 0 else max(0.0, (base - best_v) / base)
    return best_s, conf


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict((a[2:].split("=", 1) + [""])[:2] for a in sys.argv[1:] if a.startswith("--"))
    x0 = float(opts.get("x0", 0.5)); x1 = float(opts.get("x1", 1.0))
    y0 = float(opts["y0"]) if "y0" in opts else None
    y1 = float(opts["y1"]) if "y1" in opts else None
    step = int(opts.get("step", 4))
    max_shift = int(opts.get("max", 320)) // step
    if "extent" in opts:
        img = load(args[0])
        w, h, bpp, px = img
        cx0, cx1 = int(w * x0), int(w * x1)
        top, bot = surface_rows(w, h, bpp, px, cx0, cx1)
        left, right = surface_cols(w, h, bpp, px, cx0, cx1, top, bot)
        print(f"{left} {top} {right} {bot}")
        return
    ia = load(args[0]); ib = load(args[1])
    cx0, cx1, cy0, cy1 = band_for(ia, x0, x1, y0, y1)   # the band is A's; B is judged in it
    pa = profile(ia, cx0, cx1, cy0, cy1, step)
    pb = profile(ib, cx0, cx1, cy0, cy1, step)
    s_, conf = best_shift(pa, pb, max_shift)
    print(f"{s_ * step} {conf:.3f}")


if __name__ == "__main__":
    main()
