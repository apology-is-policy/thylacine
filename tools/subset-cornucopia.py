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
# Box-drawing / block elements (U+2500-259F) stay out for the same reason
# they stay out of the bake: halcyond draws them procedurally so the joins
# are pixel-exact across cells (boxglyph.rs).
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--atlas", required=True,
                    help="a baked atlas; its glyph table IS the subset set")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    cps, cell = atlas_codepoints(args.atlas)
    print(f"{args.atlas}: cell {cell[0]}x{cell[1]} baseline {cell[2]}, "
          f"{len(cps)} codepoints")

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


if __name__ == "__main__":
    sys.exit(main())
