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
        // The header must fit AND agree with the magic/version. Holotype G-4 F2:
        // ALSO validate the record-table extent (count*8 fits) and a sane cell
        // geometry -- otherwise a truncated/corrupt (but 16-byte-header-intact)
        // blob passes verify() and then PANICS on the first glyph() table read
        // (rd_u32 at HDR_LEN + mid*8) or a degenerate-geometry underflow in the
        // renderer. ATLAS is a compiled-in constant so this is build-integrity,
        // not a runtime attack surface -- but "a truncated atlas is surfaced
        // loudly" must actually hold at startup, per the doc contract.
        if ATLAS.len() < HDR_LEN || rd_u32(0) != MAGIC || rd_u32(4) != 1 {
            return false;
        }
        let count = rd_u16(14) as usize;
        // The record table (count * 8 bytes) must fit after the 16-byte header.
        if HDR_LEN + count.saturating_mul(8) > ATLAS.len() {
            return false;
        }
        // A sane cell geometry: the renderer's box/notdef math needs cell_w >= 2
        // and cell_h >= 8 (below that its saturating/subtraction bounds degrade).
        let (cw, ch) = (rd_u16(8) as usize, rd_u16(10) as usize);
        cw >= 2 && ch >= 8
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
