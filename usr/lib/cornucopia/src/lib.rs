// cornucopia -- the baked Cornucopia glyph atlases (the Thylacine system face).
//
// AURORA.md section 3: Cornucopia (the user's reconfigured Iosevka, MIT --
// github.com/apology-is-policy/cornucopia-font) is ONE outline source
// rasterized TWO ways; this crate carries the BUILD-TIME BAKE -- fixed-cell
// 8-bit-alpha bitmap atlases compiled in, so the renderer (Aurora; later the
// kernel trusted sink + Halls share the same bake via a C emission) needs no
// TTF rasterizer at runtime.
//
// SEVERAL sizes (cfg-5): the SAME outline is baked at progressively smaller
// cell advances (10 down to 6 px), so Aurora can offer a discrete font-size
// selection with no runtime rasterizer -- the config/OSD pick one by advance
// (ADVANCES). `src/atlas.bin` (advance 10, the default face) and the
// `src/atlas-N.bin` siblings are COMMITTED; regenerate with
// `tools/bake-cornucopia.py --advance N` only when the font or a cell
// geometry changes (provenance: cornucopia-Regular.ttf; advance N -> cell
// NxM, 207 glyphs each).
//
// Box-drawing/block-element codepoints (U+2500-259F) are deliberately ABSENT:
// the renderer draws them procedurally for pixel-perfect cell joins (a
// font's box glyphs are metrics-bound to ITS line box, not the cell).
//
// The atlas.bin layout is fixed little-endian (see the bake tool header);
// this parser and the tool must stay in lockstep.

#![cfg_attr(not(test), no_std)]

static A10: &[u8] = include_bytes!("atlas.bin"); // advance 10 -> 10x22 (default)
static A9: &[u8] = include_bytes!("atlas-9.bin"); // 9x20
static A8: &[u8] = include_bytes!("atlas-8.bin"); // 8x18
static A7: &[u8] = include_bytes!("atlas-7.bin"); // 7x16
static A6: &[u8] = include_bytes!("atlas-6.bin"); // 6x14

/// The baked cell advances (= cell width in px), LARGEST FIRST. The config
/// key `font-size <advance>` and the OSD Font cycler select by these; index 0
/// (`DEFAULT_ADVANCE`) is the original face. Legibility floor is 6 (below it
/// the procedural box glyphs -- which need cell_w/cell_h >= 6 -- break).
pub const ADVANCES: [u8; 5] = [10, 9, 8, 7, 6];
pub const DEFAULT_ADVANCE: u8 = 10;

const MAGIC: u32 = 0x4C54_4143; // "CATL"
const HDR_LEN: usize = 16;

/// A view over one baked atlas blob. `Copy` (a fat pointer): the renderer
/// carries it in `Metrics` and swaps it wholesale on a font-size change, so
/// the cell dims and the glyph source stay consistent by construction.
#[derive(Copy, Clone)]
pub struct Atlas {
    blob: &'static [u8],
}

impl Atlas {
    /// The atlas for a baked advance; an UNKNOWN advance falls back to the
    /// default face (config forward-compat: an old binary tolerates a future
    /// size, a new binary tolerates a dropped one -- never a panic).
    pub fn for_advance(advance: u8) -> Atlas {
        let blob = match advance {
            10 => A10,
            9 => A9,
            8 => A8,
            7 => A7,
            6 => A6,
            _ => A10,
        };
        Atlas { blob }
    }

    #[inline]
    fn rd_u32(&self, off: usize) -> u32 {
        u32::from_le_bytes([
            self.blob[off],
            self.blob[off + 1],
            self.blob[off + 2],
            self.blob[off + 3],
        ])
    }

    #[inline]
    fn rd_u16(&self, off: usize) -> u16 {
        u16::from_le_bytes([self.blob[off], self.blob[off + 1]])
    }

    pub fn verify(&self) -> bool {
        // The header must fit AND agree with the magic/version. Holotype G-4 F2:
        // ALSO validate the record-table extent (count*8 fits) and a sane cell
        // geometry -- otherwise a truncated/corrupt (but 16-byte-header-intact)
        // blob passes verify() and then PANICS on the first glyph() table read
        // (rd_u32 at HDR_LEN + mid*8) or a degenerate-geometry underflow in the
        // renderer. The blobs are compiled-in constants so this is
        // build-integrity, not a runtime attack surface -- but "a truncated
        // atlas is surfaced loudly" must actually hold at startup, per the doc
        // contract.
        if self.blob.len() < HDR_LEN || self.rd_u32(0) != MAGIC || self.rd_u32(4) != 1 {
            return false;
        }
        let count = self.rd_u16(14) as usize;
        // The record table (count * 8 bytes) must fit after the 16-byte header.
        if HDR_LEN + count.saturating_mul(8) > self.blob.len() {
            return false;
        }
        // A sane cell geometry: the renderer's box/notdef math needs cell_w >= 2
        // and cell_h >= 8 (below that its saturating/subtraction bounds degrade).
        let (cw, ch) = (self.rd_u16(8) as usize, self.rd_u16(10) as usize);
        cw >= 2 && ch >= 8
    }

    #[inline]
    pub fn cell_w(&self) -> usize {
        self.rd_u16(8) as usize
    }

    #[inline]
    pub fn cell_h(&self) -> usize {
        self.rd_u16(10) as usize
    }

    /// Rows from the cell top to the text baseline (for underline placement).
    #[inline]
    pub fn baseline(&self) -> usize {
        self.rd_u16(12) as usize
    }

    #[inline]
    fn count(&self) -> usize {
        self.rd_u16(14) as usize
    }

    /// The glyph's cell_w*cell_h alpha bytes (row-major), or None when the
    /// codepoint is not baked (the renderer falls back -- a hollow box for
    /// text, the procedural path for box drawing). Binary search over the
    /// sorted codepoint table.
    pub fn glyph(&self, ch: char) -> Option<&'static [u8]> {
        let cp = ch as u32;
        let n = self.count();
        let (mut lo, mut hi) = (0usize, n);
        while lo < hi {
            let mid = (lo + hi) / 2;
            let rec = HDR_LEN + mid * 8;
            let rcp = self.rd_u32(rec);
            if rcp == cp {
                let off = self.rd_u32(rec + 4) as usize;
                let len = self.cell_w() * self.cell_h();
                if off + len > self.blob.len() {
                    return None; // truncated atlas -- fail soft
                }
                return Some(&self.blob[off..off + len]);
            } else if rcp < cp {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        None
    }
}

/// Verify EVERY baked size at startup -- a truncated/corrupt sibling blob
/// (e.g. a re-bake that half-wrote) must be surfaced loudly, not discovered
/// on the first glyph read after a font-size cycle.
pub fn verify_all() -> bool {
    let mut i = 0;
    while i < ADVANCES.len() {
        if !Atlas::for_advance(ADVANCES[i]).verify() {
            return false;
        }
        i += 1;
    }
    true
}

// Host tests (LIVE via cfg_attr(not(test), no_std) -- cornucopia is pure
// byte-parsing over the compiled-in blobs, so unlike aurora's no_std+aarch64
// modules these run under `cargo test -p cornucopia`).
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn all_sizes_verify_and_shrink_monotonically() {
        // Every baked advance verifies, cell_w == its advance, the cells stay
        // above the procedural-box legibility floor (cell_w/cell_h >= 6), and
        // the set is strictly monotonic (largest-first) -- so the OSD cycler
        // walks a clean progression and no blob half-wrote.
        let mut prev_w = usize::MAX;
        let mut prev_h = usize::MAX;
        for &a in ADVANCES.iter() {
            let at = Atlas::for_advance(a);
            assert!(at.verify(), "advance {} fails verify", a);
            assert_eq!(at.cell_w(), a as usize, "cell_w == advance for {}", a);
            assert!(at.cell_w() >= 6 && at.cell_h() >= 6, "box-glyph floor at {}", a);
            assert!(at.cell_h() > at.baseline(), "baseline within the cell at {}", a);
            assert!(at.cell_w() < prev_w, "cell_w strictly shrinks at {}", a);
            assert!(at.cell_h() < prev_h, "cell_h strictly shrinks at {}", a);
            prev_w = at.cell_w();
            prev_h = at.cell_h();
            // A staple glyph is present and correctly sized in every atlas.
            let g = at.glyph('A').expect("'A' baked");
            assert_eq!(g.len(), at.cell_w() * at.cell_h());
        }
        assert!(verify_all());
    }

    #[test]
    fn default_is_the_largest_and_unknown_falls_back() {
        assert_eq!(ADVANCES[0], DEFAULT_ADVANCE);
        assert_eq!(Atlas::for_advance(DEFAULT_ADVANCE).cell_w(), 10);
        // An unknown advance falls back to the default face (forward-compat).
        assert_eq!(Atlas::for_advance(99).cell_w(), 10);
        assert_eq!(Atlas::for_advance(0).cell_w(), 10);
    }
}
