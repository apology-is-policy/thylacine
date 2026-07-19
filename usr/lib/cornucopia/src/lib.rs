// cornucopia -- the baked Cornucopia glyph atlas (the Thylacine system face).
//
// AURORA.md section 3: Cornucopia (the user's reconfigured Iosevka, MIT --
// github.com/apology-is-policy/cornucopia-font) is ONE outline source
// rasterized TWO ways; this crate carries the BUILD-TIME BAKE -- a
// fixed-cell 8-bit-alpha bitmap atlas compiled in, so the renderer (Aurora;
// later the kernel trusted sink + Halls share the same bake via a C
// emission) needs no TTF rasterizer at runtime. `src/atlas.bin` is COMMITTED
// -- regenerate with `tools/bake-cornucopia.py` only when the font or the
// cell geometry changes (provenance: cornucopia-Regular.ttf, advance 10 ->
// cell 10x22, baseline 18, 207 glyphs).
//
// Box-drawing/block-element codepoints (U+2500-259F) are deliberately ABSENT:
// the renderer draws them procedurally for pixel-perfect cell joins (a
// font's box glyphs are metrics-bound to ITS line box, not the cell).
//
// The atlas.bin layout is fixed little-endian (see the bake tool header);
// this parser and the tool must stay in lockstep.

#![no_std]

static ATLAS: &[u8] = include_bytes!("atlas.bin");

const MAGIC: u32 = 0x4C54_4143; // "CATL"
const HDR_LEN: usize = 16;

#[inline]
fn rd_u32(off: usize) -> u32 {
    u32::from_le_bytes([ATLAS[off], ATLAS[off + 1], ATLAS[off + 2], ATLAS[off + 3]])
}

#[inline]
fn rd_u16(off: usize) -> u16 {
    u16::from_le_bytes([ATLAS[off], ATLAS[off + 1]])
}

/// The atlas view. Zero-init: all accessors read the committed bytes
/// directly; `verify()` checks the magic/version once at startup (a
/// mismatched or truncated atlas is a build error, surfaced loudly).
pub struct Atlas;

impl Atlas {
    pub fn verify() -> bool {
        ATLAS.len() >= HDR_LEN && rd_u32(0) == MAGIC && rd_u32(4) == 1
    }

    #[inline]
    pub fn cell_w() -> usize {
        rd_u16(8) as usize
    }

    #[inline]
    pub fn cell_h() -> usize {
        rd_u16(10) as usize
    }

    /// Rows from the cell top to the text baseline (for underline placement).
    #[inline]
    pub fn baseline() -> usize {
        rd_u16(12) as usize
    }

    #[inline]
    fn count() -> usize {
        rd_u16(14) as usize
    }

    /// The glyph's cell_w*cell_h alpha bytes (row-major), or None when the
    /// codepoint is not baked (the renderer falls back -- a hollow box for
    /// text, the procedural path for box drawing). Binary search over the
    /// sorted codepoint table.
    pub fn glyph(ch: char) -> Option<&'static [u8]> {
        let cp = ch as u32;
        let n = Self::count();
        let (mut lo, mut hi) = (0usize, n);
        while lo < hi {
            let mid = (lo + hi) / 2;
            let rec = HDR_LEN + mid * 8;
            let rcp = rd_u32(rec);
            if rcp == cp {
                let off = rd_u32(rec + 4) as usize;
                let len = Self::cell_w() * Self::cell_h();
                if off + len > ATLAS.len() {
                    return None; // truncated atlas -- fail soft
                }
                return Some(&ATLAS[off..off + len]);
            } else if rcp < cp {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        None
    }
}
