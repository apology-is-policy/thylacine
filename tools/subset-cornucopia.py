#!/usr/bin/env python3
# subset-cornucopia.py -- cut the Cornucopia TTF down to the codepoints the
# bake carries, for halcyond's LIVE mono path (HALCYON-TYPE.md section 4.5).
#
# The sibling of tools/bake-cornucopia.py: same font, same codepoints, the
# other of the two ways AURORA.md section 3 rasterizes one outline source.
# The bake serves Aurora / the kernel trusted sink / Halls, which must stay
# TTF-rasterizer-free; halcyond is userspace and rasterizes the outline at
# runtime, so it needs the outline -- but not 10.8 MB of it.
#
#   python3 -m venv /tmp/fontenv && /tmp/fontenv/bin/pip install fonttools
#   /tmp/fontenv/bin/python3 tools/subset-cornucopia.py \
#       --ttf ~/projects/cornucopia-font/cornucopia-Regular.ttf \
#       --atlas usr/lib/cornucopia/src/atlas.bin \
#       --out usr/lib/cornucopia/src/cornucopia-subset.ttf
#
# The codepoint list is READ OUT OF THE BAKED ATLAS, not restated here. A
# constant copied into two tools is a constant that drifts; the atlas is the
# artifact the other tier actually serves, so taking the set from it makes
# "the two tiers carry the same glyphs" true by construction rather than by
# maintenance. It also inherits the bake's own exclusions for free -- a
# codepoint the font lacks, or one whose advance is not the monospace
# advance, never entered the atlas and so never enters the subset.
#
# `--extra` ADDS codepoints the bake does not carry (HALCYON-INSTRUMENT
# 7.1, I-5): the subset is then a SUPERSET of the atlas -- every baked
# codepoint is still in it (halcyond's test holds), and the extras serve
# only the outline tier. The Instrument set is the six glyphs its surfaces
# use (lambda, the check mark, the angle quotes, minus, the command glyph)
# plus the box-drawing block U+2500-257F:
#
#   --extra 03BB,2713,2039,203A,2212,2318,2500-257F
#
# The box-drawing glyphs are in the SUBSET but NOT in the bake, and the cell
# path never uses them: halcyond draws U+2500-259F procedurally on the cell
# so the joins are pixel-exact (boxglyph, consulted BEFORE the face), which
# is why the bake omits them. They are cut in for the free-running mono
# path (a chrome run at the type map's 10/11 px, no cell), where a font
# glyph is the right thing and a procedural cell glyph does not exist.
#
# `--match <ttf>` cuts a SECOND FACE to the same codepoints (the Italic,
# ruling 11) and refuses unless its cell-bearing tables -- upem, the OS/2
# Windows ascent/descent, the advance of 'x' -- equal the given face's, so
# the italic lands in the Regular's cell by construction:
#
#   --ttf ~/projects/cornucopia-font/cornucopia-Italic.ttf \
#       --match usr/lib/cornucopia/src/cornucopia-subset.ttf \
#       --extra ... --out usr/lib/cornucopia/src/cornucopia-subset-italic.ttf
#
# The output is a COMMITTED generated artifact, beside the atlases it was
# cut alongside -- not a third_party vendoring, which is reserved for
# byte-for-byte upstream copies (third_party/README.md). Re-run it only when
# the font or the bake's codepoint set changes, and re-run the bake in the
# same commit so the two stay the same font.

import argparse
import struct
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

MAGIC = 0x4C544143  # "CATL"
HDR = "<II4H"  # magic, version, cell_w, cell_h, baseline, count


def atlas_codepoints(path):
    """The codepoint set of a baked atlas, with its cell geometry."""
    with open(path, "rb") as f:
        blob = f.read()
    magic, ver, cell_w, cell_h, baseline, count = struct.unpack_from(HDR, blob, 0)
    if magic != MAGIC or ver != 1:
        raise SystemExit(f"{path}: not a v1 CATL atlas "
                         f"(magic {magic:#x}, version {ver})")
    cps = [struct.unpack_from("<II", blob, 16 + i * 8)[0] for i in range(count)]
    return cps, (cell_w, cell_h, baseline)


def parse_extra(text):
    """`03BB,2713,2500-257F` -> the codepoints named (hex; LO-HI ranges)."""
    out = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            lo, hi = item.split("-", 1)
            lo, hi = int(lo, 16), int(hi, 16)
            if hi < lo:
                raise SystemExit(f"--extra: empty range {item}")
            out.extend(range(lo, hi + 1))
        else:
            out.append(int(item, 16))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--atlas", required=True,
                    help="a baked atlas; its glyph table IS the subset set")
    ap.add_argument("--out", required=True)
    ap.add_argument("--extra", default="",
                    help="codepoints to ADD beyond the atlas: hex, comma-"
                         "separated, ranges as LO-HI (e.g. 03BB,2500-257F)")
    ap.add_argument("--match", default=None,
                    help="a cut face whose cell-bearing tables this cut "
                         "must equal (the Italic against the Regular)")
    args = ap.parse_args()

    cps, cell = atlas_codepoints(args.atlas)
    print(f"{args.atlas}: cell {cell[0]}x{cell[1]} baseline {cell[2]}, "
          f"{len(cps)} codepoints")
    extra = parse_extra(args.extra)
    if extra:
        print(f"extra: {len(extra)} codepoints beyond the atlas")
    cps = sorted(set(cps) | set(extra))

    font = TTFont(args.ttf)
    cmap = font.getBestCmap()
    missing = [c for c in cps if c not in cmap]
    if missing:
        raise SystemExit("the font lacks codepoints the atlas carries -- the "
                         "two were cut from different fonts: "
                         + ", ".join("U+%04X" % c for c in missing))

    # Keep exactly what the runtime rasterizer reads: outlines (glyf/loca),
    # the character map, the horizontal metrics, and OS/2 -- whose winAscent
    # / winDescent are the cell height and baseline (bake-cornucopia.py's
    # formula, which halcyond re-derives from these same fields). Everything
    # else -- names, hinting programs, layout tables, kerning -- is dead
    # weight for a monospace cell renderer with no hinter and no shaper.
    opts = subset.Options()
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "kern", "DSIG", "FFTM"]
    opts.hinting = False
    opts.legacy_kern = False
    opts.name_IDs = []
    opts.name_legacy = False
    opts.notdef_outline = True  # the .notdef box is a legitimate render
    opts.glyf_prune_unnamed = True
    opts.recalc_bounds = True
    opts.desubroutinize = False
    opts.layout_features = []

    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=cps)
    subsetter.subset(font)

    font.flavor = None  # plain TTF: skrifa reads it without a decompressor
    font.save(args.out)

    out = TTFont(args.out)
    kept = out.getBestCmap()
    os2 = out["OS/2"]
    adv = out["hmtx"][kept[ord("x")]][0]
    import os
    print(f"subset {len(kept)} codepoints, upem {out['head'].unitsPerEm}, "
          f"advance {adv}, winAscent {os2.usWinAscent}, "
          f"winDescent {os2.usWinDescent} -> {args.out} "
          f"({os.path.getsize(args.out)} bytes, from "
          f"{os.path.getsize(args.ttf)})")
    short = sorted(set(cps) - set(kept))
    if short:
        raise SystemExit("the subset dropped codepoints the atlas carries: "
                         + ", ".join("U+%04X" % c for c in short))
    if args.match:
        ref = TTFont(args.match)
        rcmap = ref.getBestCmap()
        want = (ref["head"].unitsPerEm, ref["OS/2"].usWinAscent,
                ref["OS/2"].usWinDescent, ref["hmtx"][rcmap[ord("x")]][0])
        got = (out["head"].unitsPerEm, os2.usWinAscent, os2.usWinDescent, adv)
        if want != got:
            raise SystemExit(f"{args.out}: cell-bearing tables (upem, "
                             f"winAscent, winDescent, x advance) {got} != "
                             f"{args.match}'s {want}: the two faces would "
                             "not share a cell")
        rk = set(rcmap)
        if set(kept) != rk:
            raise SystemExit(f"{args.out}: codepoint set differs from "
                             f"{args.match}'s (+{len(set(kept) - rk)} "
                             f"-{len(rk - set(kept))})")
        print(f"matches {args.match}: the same cell tables, the same "
              f"{len(kept)} codepoints")


if __name__ == "__main__":
    sys.exit(main())
