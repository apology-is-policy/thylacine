#!/usr/bin/env python3
# bake-cornucopia.py -- rasterize the Cornucopia TTF into the fixed-cell
# bitmap atlas Aurora (and, later, the kernel trusted sink + Halls) blits.
#
# AURORA.md section 3 / memory reference-cornucopia-font: Cornucopia (the
# user's reconfigured Iosevka; MIT) is ONE outline source rasterized TWO ways.
# This tool is the build-time bake: TTF -> an 8-bit-alpha glyph atlas at one
# fixed cell size, COMMITTED to the tree (usr/lib/cornucopia/src/atlas.bin) so
# ordinary builds never need font tooling. Re-run only when the font or the
# cell geometry changes:
#
#   python3 -m venv /tmp/fontenv && /tmp/fontenv/bin/pip install fonttools
#   /tmp/fontenv/bin/python3 tools/bake-cornucopia.py \
#       --ttf ~/projects/cornucopia-font/cornucopia-Regular.ttf \
#       --out usr/lib/cornucopia/src/atlas.bin
#
# The bake set: printable ASCII + Latin-1 supplement + a small extras list
# (the ut prompt glyphs). Box-drawing/block-element glyphs are DELIBERATELY
# NOT baked -- Aurora draws U+2500-259F procedurally for pixel-perfect cell
# joins (the kitty/alacritty approach; a font's box glyphs are metrics-bound
# to ITS line box, not our cell).
#
# Rasterization: flatten the TrueType outlines (quadratics -> segments) and
# scanline-fill with NONZERO winding at 4x supersample, then box-filter down
# to 8-bit alpha. No hinting -- the supersampled anti-aliasing carries the
# quality at the 20px em this bakes.
#
# atlas.bin layout (little-endian; parsed by usr/lib/cornucopia/src/lib.rs --
# keep the two in lockstep):
#   0   u32  magic "CATL" (0x4C544143)
#   4   u32  version = 1
#   8   u16  cell_w
#   10  u16  cell_h
#   12  u16  baseline    (rows from cell top to the text baseline)
#   14  u16  count       (glyph records)
#   16  count * { u32 codepoint, u32 offset }   (sorted by codepoint;
#                offset is from the FILE start to cell_w*cell_h alpha bytes)
#   ...  bitmaps

import argparse
import math
import struct
import sys

from fontTools.ttLib import TTFont
from fontTools.pens.basePen import BasePen

SS = 4  # supersample factor


class FlattenPen(BasePen):
    """Record contours as flattened point lists in supersampled pixel space."""

    def __init__(self, glyphSet, scale, baseline_px_ss):
        super().__init__(glyphSet)
        self.scale = scale                    # units -> supersampled px
        self.base = baseline_px_ss            # baseline row in ss px (y-down)
        self.contours = []
        self.cur = None

    def _pt(self, p):
        x, y = p
        return (x * self.scale, self.base - y * self.scale)

    def _moveTo(self, p):
        self.cur = [self._pt(p)]

    def _lineTo(self, p):
        self.cur.append(self._pt(p))

    def _qCurveToOne(self, p1, p2):
        # One quadratic segment (BasePen decomposes multi-off-curve qCurveTo
        # into these): flatten with fixed subdivision.
        x0, y0 = self.cur[-1]
        cx, cy = self._pt(p1)
        x2, y2 = self._pt(p2)
        n = 8
        for i in range(1, n + 1):
            t = i / n
            mt = 1.0 - t
            self.cur.append((mt * mt * x0 + 2 * mt * t * cx + t * t * x2,
                             mt * mt * y0 + 2 * mt * t * cy + t * t * y2))

    def _curveToOne(self, p1, p2, p3):
        x0, y0 = self.cur[-1]
        c1x, c1y = self._pt(p1)
        c2x, c2y = self._pt(p2)
        x3, y3 = self._pt(p3)
        n = 12
        for i in range(1, n + 1):
            t = i / n
            mt = 1.0 - t
            self.cur.append((mt**3 * x0 + 3 * mt * mt * t * c1x
                             + 3 * mt * t * t * c2x + t**3 * x3,
                             mt**3 * y0 + 3 * mt * mt * t * c1y
                             + 3 * mt * t * t * c2y + t**3 * y3))

    def _closePath(self):
        if self.cur and len(self.cur) >= 2:
            self.contours.append(self.cur)
        self.cur = None


def rasterize(contours, w_ss, h_ss):
    """Nonzero-winding scanline fill; returns bytearray of w_ss*h_ss 0/1."""
    grid = bytearray(w_ss * h_ss)
    # Build the edge list once: (y0, y1, x_at, slope, winding)
    edges = []
    for poly in contours:
        n = len(poly)
        for i in range(n):
            x0, y0 = poly[i]
            x1, y1 = poly[(i + 1) % n]
            if y0 == y1:
                continue
            edges.append((x0, y0, x1, y1))
    for row in range(h_ss):
        yc = row + 0.5
        xs = []
        for (x0, y0, x1, y1) in edges:
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                t = (yc - y0) / (y1 - y0)
                x = x0 + t * (x1 - x0)
                w = 1 if y1 > y0 else -1
                xs.append((x, w))
        if not xs:
            continue
        xs.sort()
        winding = 0
        spans = []
        span_start = None
        for (x, w) in xs:
            prev = winding
            winding += w
            if prev == 0 and winding != 0:
                span_start = x
            elif prev != 0 and winding == 0 and span_start is not None:
                spans.append((span_start, x))
                span_start = None
        for (sx, ex) in spans:
            a = max(0, int(math.ceil(sx - 0.5)))
            b = min(w_ss - 1, int(math.floor(ex - 0.5)))
            if b < a:
                continue
            base = row * w_ss
            for px in range(a, b + 1):
                grid[base + px] = 1
    return grid


def downsample(grid, w_ss, h_ss, w, h):
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            s = 0
            for dy in range(SS):
                row = (y * SS + dy) * w_ss
                for dx in range(SS):
                    s += grid[row + x * SS + dx]
            out[y * w + x] = (s * 255) // (SS * SS)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--advance", type=int, default=10,
                    help="cell width in px (em = advance * upm / adv_units)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    font = TTFont(args.ttf)
    upm = font["head"].unitsPerEm
    os2 = font["OS/2"]
    cmap = font.getBestCmap()
    glyph_set = font.getGlyphSet()
    hmtx = font["hmtx"]

    # Cornucopia is strictly monospace (advance 500/1000 everywhere we bake);
    # derive the scale from the actual advance so the cell tiles exactly.
    adv_units = hmtx[cmap[ord("x")]][0]
    scale = args.advance / adv_units          # units -> px
    cell_w = args.advance
    # Vertical extents from the OS/2 win metrics (the outermost ink bounds).
    cell_h = math.ceil((os2.usWinAscent + os2.usWinDescent) * scale)
    baseline = math.ceil(os2.usWinAscent * scale)

    codepoints = list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [
        0x22A2,  # RIGHT TACK (the ut prompt glyph)
        0x22EE,  # VERTICAL ELLIPSIS (the ut continuation glyph)
        0x2026,  # HORIZONTAL ELLIPSIS
        0x2013, 0x2014,          # en/em dash
        0x2018, 0x2019, 0x201C, 0x201D,  # curly quotes
        0x2022,  # bullet
        0x2190, 0x2191, 0x2192, 0x2193,  # arrows
        0x25CF, 0x25CB,          # filled/hollow circle
    ]

    w_ss, h_ss = cell_w * SS, cell_h * SS
    records = []
    bitmaps = []
    skipped = []
    for cp in sorted(set(codepoints)):
        gname = cmap.get(cp)
        if gname is None:
            skipped.append(cp)
            continue
        # Uniform-advance sanity: a proportional glyph would break the grid.
        if hmtx[gname][0] != adv_units:
            skipped.append(cp)
            continue
        pen = FlattenPen(glyph_set, scale * SS, baseline * SS)
        glyph_set[gname].draw(pen)
        if pen.cur:  # unclosed trailing contour (defensive)
            pen.contours.append(pen.cur)
        grid = rasterize(pen.contours, w_ss, h_ss)
        bitmaps.append(bytes(downsample(grid, w_ss, h_ss, cell_w, cell_h)))
        records.append(cp)

    header = struct.pack("<II4H", 0x4C544143, 1, cell_w, cell_h, baseline,
                         len(records))
    table_size = len(records) * 8
    off = len(header) + table_size
    table = b""
    for i, cp in enumerate(records):
        table += struct.pack("<II", cp, off + i * cell_w * cell_h)

    with open(args.out, "wb") as f:
        f.write(header)
        f.write(table)
        for bm in bitmaps:
            f.write(bm)

    print(f"baked {len(records)} glyphs, cell {cell_w}x{cell_h} baseline "
          f"{baseline}, {len(header) + table_size + len(records) * cell_w * cell_h}"
          f" bytes -> {args.out}")
    if skipped:
        print(f"skipped (no glyph / non-uniform advance): "
              f"{['U+%04X' % c for c in skipped]}")


if __name__ == "__main__":
    sys.exit(main())
