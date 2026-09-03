// The VT interpreter: a byte stream in, a cell grid out (AURORA.md section 3
// -- Aurora is the SCREEN-side of the terminal protocol; Kaua is the
// app-side emitter). The subset covers what the tree's emitters produce
// (libutopia's ANSI + truecolor SGR, Kaua's cursor/erase/alt-screen, login's
// plain lines) plus the classic VT100 core; unknown sequences are parsed and
// dropped (never desync). DECSTBM scroll regions are accepted-and-ignored
// (full-screen scroll only) -- the recorded MVP seam.
//
// Colors are the Bonfire palette (docs/UTOPIA-VISUAL.md section 1): default
// bg/fg + the role-derived ANSI-16 map below. SGR 38;2 truecolor passes
// through exactly (libutopia emits it), 38;5 maps via the xterm-256 cube.
//
// A shared crate since H-2a (HALCYON.md section 13.4): aurora hosts one Vt
// per surface; halcyond hosts one per raw-VT pane plus the transcript's
// inline SGR-subset interpreter. Pure no_std + alloc -- the parser is host-
// tested (cargo test -p vt --target aarch64-apple-darwin); the pixel side
// (render.rs, the atlas blit) stays with each consumer.

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

// Bonfire (UTOPIA-VISUAL.md section 1). The 16-color map derives from the
// section-1.4 role table (slate=blue, sage=cyan, sand=yellow, moss/fen=green,
// dusk=magenta, cinnabar=red); the bright tier lifts each role toward fg
// (brights are not pinned by scripture -- these are Aurora's derivation,
// documented here).
pub const BG: u32 = 0xFF0E_0C0C;
pub const FG: u32 = 0xFFE4_DDD8;
const ANSI: [u32; 16] = [
    0xFF2A_1F1C, // 0 black   (gutter -- visible against bg)
    0xFFC0_6050, // 1 red     (cinnabar)
    0xFF6A_9A6A, // 2 green   (fen)
    0xFFC8_A882, // 3 yellow  (sand)
    0xFF8A_9AC8, // 4 blue    (slate)
    0xFFA8_98C8, // 5 magenta (dusk)
    0xFF8A_B8A8, // 6 cyan    (sage)
    0xFFC8_BDB8, // 7 white   (fg_dim)
    0xFF5A_4E48, // 8 bright black (fg_subtle)
    0xFFE0_7840, // 9 bright red   (ember -- the fire)
    0xFFB8_D098, // 10 bright green (moss)
    0xFFE0_C090, // 11 bright yellow
    0xFFA0_B0E0, // 12 bright blue
    0xFFC0_B0E0, // 13 bright magenta
    0xFFA0_D0C0, // 14 bright cyan
    0xFFE4_DDD8, // 15 bright white (fg)
];

// The palette as runtime state (AURORA-CONFIG.md section 3.6: `theme` is an
// aurora-local setting the OSD live-applies -- no gate, no compositor
// involvement). Cells bake RESOLVED colors at write time, so a theme switch
// remaps existing cells by exact old->new color match (set_theme); truecolor
// SGR passes through a switch untouched, by design.
#[derive(Clone, Copy, PartialEq)]
pub struct Palette {
    pub bg: u32,
    pub fg: u32,
    pub ansi: [u32; 16],
}

pub const BONFIRE: Palette = Palette { bg: BG, fg: FG, ansi: ANSI };

// Parchment: the light counterpart (warm paper + ink; the ANSI tier darkened
// for contrast on a light field). Values are Aurora's derivation, like the
// Bonfire brights above.
// Slot-uniqueness note (the set_theme exact-match remap): within a palette,
// no two slots may share a value EXCEPT ansi[15] == fg (deliberate + present
// in every theme, so the alias maps consistently across any switch). A
// one-sided alias (a slot aliasing fg in one theme but not another) would
// mis-slot cells on the round-trip -- ansi[0] below is deliberately distinct
// from fg for exactly that reason.
const PARCHMENT: Palette = Palette {
    bg: 0xFFF1_EAE0,
    fg: 0xFF2B_2320,
    ansi: [
        0xFF3A_332E, // 0 black (distinct from fg -- see the slot-uniqueness note)
        0xFF9C_3A28, // 1 red
        0xFF3F_6B3F, // 2 green
        0xFF8A_6520, // 3 yellow (dark gold)
        0xFF3A_4E86, // 4 blue
        0xFF6A_4E86, // 5 magenta
        0xFF2F_6E62, // 6 cyan
        0xFF6B_5F58, // 7 white (dim ink)
        0xFF9A_8C82, // 8 bright black
        0xFFB4_472E, // 9 bright red
        0xFF4E_8046, // 10 bright green
        0xFFA0_7828, // 11 bright yellow
        0xFF4A_62A8, // 12 bright blue
        0xFF7E_5EA0, // 13 bright magenta
        0xFF3A_8578, // 14 bright cyan
        0xFF2B_2320, // 15 bright white (= fg)
    ],
};

// Spinifex: green phosphor over near-black (the Tasmanian-bushland name),
// hues kept distinguishable (errors must still read red).
const SPINIFEX: Palette = Palette {
    bg: 0xFF0A_0F0A,
    fg: 0xFF9F_D89A,
    ansi: [
        0xFF1E_2A1E, // 0 black
        0xFFC8_6850, // 1 red
        0xFF5F_A85F, // 2 green
        0xFFB8_B070, // 3 yellow
        0xFF7F_A0A8, // 4 blue (steel)
        0xFF9A_98B8, // 5 magenta (lavender-sage)
        0xFF78_B8A0, // 6 cyan
        0xFF8F_BF8A, // 7 white (dim phosphor)
        0xFF4A_5A48, // 8 bright black
        0xFFE0_7850, // 9 bright red
        0xFF88_D080, // 10 bright green
        0xFFD0_C878, // 11 bright yellow
        0xFF98_C0C8, // 12 bright blue
        0xFFB8_B0D8, // 13 bright magenta
        0xFF98_D8C0, // 14 bright cyan
        0xFF9F_D89A, // 15 bright white (= fg)
    ],
};

pub static THEMES: [(&str, Palette); 3] = [
    ("bonfire", BONFIRE),
    ("parchment", PARCHMENT),
    ("spinifex", SPINIFEX),
];

pub const ATTR_REVERSE: u8 = 1 << 0;
pub const ATTR_UNDERLINE: u8 = 1 << 1;
pub const ATTR_BOLD: u8 = 1 << 2;
pub const ATTR_ITALIC: u8 = 1 << 3;
pub const ATTR_DIM: u8 = 1 << 4;
pub const ATTR_BLINK: u8 = 1 << 5;
pub const ATTR_STRIKE: u8 = 1 << 6;

/// Cell geometry, not an SGR pen attr: the parser sets it on the LEFT half of
/// a double-width glyph so a consumer draws it two cells wide and skips the
/// next (blank) continuation cell. It lives above the SGR pen bits (which the
/// SgrPen owns, 0..=6) so the pen never sets it.
pub const ATTR_WIDE: u8 = 1 << 7;

// Cell display width (a wcwidth over Unicode scalars -- the parser decodes
// UTF-8 before put_char, so this sees full code points). 0 = combining /
// zero-width, 2 = East Asian Wide/Fullwidth + emoji, else 1. A hostile-input
// surface: total over every char, never panics. Ranges follow Markus Kuhn's
// reference wcwidth (the de-facto terminal standard) plus the supplementary
// emoji planes; obscure combining marks outside the table fall back to width 1
// -- a minor rendering imperfection, never a misaccounting of the common case.
fn in_ranges(cp: u32, table: &[(u32, u32)]) -> bool {
    let (mut lo, mut hi) = (0usize, table.len());
    while lo < hi {
        let mid = (lo + hi) / 2;
        let (a, b) = table[mid];
        if cp < a {
            hi = mid;
        } else if cp > b {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    false
}

fn is_wide(cp: u32) -> bool {
    cp >= 0x1100
        && (cp <= 0x115F // Hangul Jamo (leading)
            || cp == 0x2329
            || cp == 0x232A
            || ((0x2E80..=0xA4CF).contains(&cp) && cp != 0x303F) // CJK radicals..Yi
            || (0xAC00..=0xD7A3).contains(&cp) // Hangul Syllables
            || (0xF900..=0xFAFF).contains(&cp) // CJK Compat Ideographs
            || (0xFE10..=0xFE19).contains(&cp) // Vertical forms
            || (0xFE30..=0xFE6F).contains(&cp) // CJK Compat Forms
            || (0xFF00..=0xFF60).contains(&cp) // Fullwidth forms
            || (0xFFE0..=0xFFE6).contains(&cp) // Fullwidth signs
            || (0x1F300..=0x1FAFF).contains(&cp) // emoji pictographs
            || (0x20000..=0x3FFFD).contains(&cp)) // CJK Ext B..G
}

fn char_width(c: char) -> usize {
    let cp = c as u32;
    if cp == 0 {
        return 0;
    }
    if in_ranges(cp, COMBINING) {
        return 0;
    }
    if is_wide(cp) {
        return 2;
    }
    1
}

// Sorted, non-overlapping inclusive nonspacing-mark ranges (Kuhn's combining
// table, BMP-complete + the high-value supplementary marks + variation
// selectors). The sort/non-overlap invariant that in_ranges' binary search
// relies on is asserted by combining_table_is_sorted.
const COMBINING: &[(u32, u32)] = &[
    (0x0300, 0x036F), (0x0483, 0x0489), (0x0591, 0x05BD), (0x05BF, 0x05BF),
    (0x05C1, 0x05C2), (0x05C4, 0x05C5), (0x05C7, 0x05C7), (0x0610, 0x061A),
    (0x064B, 0x065F), (0x0670, 0x0670), (0x06D6, 0x06DC), (0x06DF, 0x06E4),
    (0x06E7, 0x06E8), (0x06EA, 0x06ED), (0x0711, 0x0711), (0x0730, 0x074A),
    (0x07A6, 0x07B0), (0x07EB, 0x07F3), (0x0816, 0x0819), (0x081B, 0x0823),
    (0x0825, 0x0827), (0x0829, 0x082D), (0x0859, 0x085B), (0x08E3, 0x0902),
    (0x093A, 0x093A), (0x093C, 0x093C), (0x0941, 0x0948), (0x094D, 0x094D),
    (0x0951, 0x0957), (0x0962, 0x0963), (0x0981, 0x0981), (0x09BC, 0x09BC),
    (0x09C1, 0x09C4), (0x09CD, 0x09CD), (0x09E2, 0x09E3), (0x0A01, 0x0A02),
    (0x0A3C, 0x0A3C), (0x0A41, 0x0A42), (0x0A47, 0x0A48), (0x0A4B, 0x0A4D),
    (0x0A70, 0x0A71), (0x0A75, 0x0A75), (0x0A81, 0x0A82), (0x0ABC, 0x0ABC),
    (0x0AC1, 0x0AC5), (0x0AC7, 0x0AC8), (0x0ACD, 0x0ACD), (0x0AE2, 0x0AE3),
    (0x0B01, 0x0B01), (0x0B3C, 0x0B3C), (0x0B3F, 0x0B3F), (0x0B41, 0x0B44),
    (0x0B4D, 0x0B4D), (0x0B56, 0x0B56), (0x0B62, 0x0B63), (0x0B82, 0x0B82),
    (0x0BC0, 0x0BC0), (0x0BCD, 0x0BCD), (0x0C00, 0x0C00), (0x0C3E, 0x0C40),
    (0x0C46, 0x0C48), (0x0C4A, 0x0C4D), (0x0C55, 0x0C56), (0x0C62, 0x0C63),
    (0x0C81, 0x0C81), (0x0CBC, 0x0CBC), (0x0CBF, 0x0CBF), (0x0CC6, 0x0CC6),
    (0x0CCC, 0x0CCD), (0x0CE2, 0x0CE3), (0x0D01, 0x0D01), (0x0D41, 0x0D44),
    (0x0D4D, 0x0D4D), (0x0D62, 0x0D63), (0x0DCA, 0x0DCA), (0x0DD2, 0x0DD4),
    (0x0DD6, 0x0DD6), (0x0E31, 0x0E31), (0x0E34, 0x0E3A), (0x0E47, 0x0E4E),
    (0x0EB1, 0x0EB1), (0x0EB4, 0x0EB9), (0x0EBB, 0x0EBC), (0x0EC8, 0x0ECD),
    (0x0F18, 0x0F19), (0x0F35, 0x0F35), (0x0F37, 0x0F37), (0x0F39, 0x0F39),
    (0x0F71, 0x0F7E), (0x0F80, 0x0F84), (0x0F86, 0x0F87), (0x0F8D, 0x0F97),
    (0x0F99, 0x0FBC), (0x0FC6, 0x0FC6), (0x102D, 0x1030), (0x1032, 0x1037),
    (0x1039, 0x103A), (0x103D, 0x103E), (0x1058, 0x1059), (0x105E, 0x1060),
    (0x1071, 0x1074), (0x1082, 0x1082), (0x1085, 0x1086), (0x108D, 0x108D),
    (0x109D, 0x109D), (0x135D, 0x135F), (0x1712, 0x1714), (0x1732, 0x1734),
    (0x1752, 0x1753), (0x1772, 0x1773), (0x17B4, 0x17B5), (0x17B7, 0x17BD),
    (0x17C6, 0x17C6), (0x17C9, 0x17D3), (0x17DD, 0x17DD), (0x180B, 0x180D),
    (0x1885, 0x1886), (0x18A9, 0x18A9), (0x1920, 0x1922), (0x1927, 0x1928),
    (0x1932, 0x1932), (0x1939, 0x193B), (0x1A17, 0x1A18), (0x1A1B, 0x1A1B),
    (0x1A56, 0x1A56), (0x1A58, 0x1A5E), (0x1A60, 0x1A60), (0x1A62, 0x1A62),
    (0x1A65, 0x1A6C), (0x1A73, 0x1A7C), (0x1A7F, 0x1A7F), (0x1AB0, 0x1ABE),
    (0x1B00, 0x1B03), (0x1B34, 0x1B34), (0x1B36, 0x1B3A), (0x1B3C, 0x1B3C),
    (0x1B42, 0x1B42), (0x1B6B, 0x1B73), (0x1B80, 0x1B81), (0x1BA2, 0x1BA5),
    (0x1BA8, 0x1BA9), (0x1BAB, 0x1BAD), (0x1BE6, 0x1BE6), (0x1BE8, 0x1BE9),
    (0x1BED, 0x1BED), (0x1BEF, 0x1BF1), (0x1C2C, 0x1C33), (0x1C36, 0x1C37),
    (0x1CD0, 0x1CD2), (0x1CD4, 0x1CE0), (0x1CE2, 0x1CE8), (0x1CED, 0x1CED),
    (0x1CF4, 0x1CF4), (0x1CF8, 0x1CF9), (0x1DC0, 0x1DFF), (0x20D0, 0x20F0),
    (0x2CEF, 0x2CF1), (0x2D7F, 0x2D7F), (0x2DE0, 0x2DFF), (0x302A, 0x302D),
    (0x3099, 0x309A), (0xA66F, 0xA672), (0xA674, 0xA67D), (0xA69E, 0xA69F),
    (0xA6F0, 0xA6F1), (0xA802, 0xA802), (0xA806, 0xA806), (0xA80B, 0xA80B),
    (0xA825, 0xA826), (0xA8C4, 0xA8C5), (0xA8E0, 0xA8F1), (0xA926, 0xA92D),
    (0xA947, 0xA951), (0xA980, 0xA982), (0xA9B3, 0xA9B3), (0xA9B6, 0xA9B9),
    (0xA9BC, 0xA9BC), (0xAA29, 0xAA2E), (0xAA31, 0xAA32), (0xAA35, 0xAA36),
    (0xAA43, 0xAA43), (0xAA4C, 0xAA4C), (0xAAB0, 0xAAB0), (0xAAB2, 0xAAB4),
    (0xAAB7, 0xAAB8), (0xAABE, 0xAABF), (0xAAC1, 0xAAC1), (0xAAEC, 0xAAED),
    (0xAAF6, 0xAAF6), (0xABE5, 0xABE5), (0xABE8, 0xABE8), (0xABED, 0xABED),
    (0xFB1E, 0xFB1E), (0xFE00, 0xFE0F), (0xFE20, 0xFE2F), (0x101FD, 0x101FD),
    (0x102E0, 0x102E0), (0x10376, 0x1037A), (0x10A01, 0x10A03), (0x10A05, 0x10A06),
    (0x10A0C, 0x10A0F), (0x10A38, 0x10A3A), (0x10A3F, 0x10A3F), (0x10AE5, 0x10AE6),
    (0x11001, 0x11001), (0x11038, 0x11046), (0x1107F, 0x11081), (0x110B3, 0x110B6),
    (0x110B9, 0x110BA), (0x11100, 0x11102), (0x11127, 0x1112B), (0x1112D, 0x11134),
    (0x11173, 0x11173), (0x11180, 0x11181), (0x111B6, 0x111BE), (0x116AB, 0x116AB),
    (0x116AD, 0x116AD), (0x116B0, 0x116B5), (0x116B7, 0x116B7), (0x16AF0, 0x16AF4),
    (0x16B30, 0x16B36), (0x16F8F, 0x16F92), (0x1D167, 0x1D169), (0x1D17B, 0x1D182),
    (0x1D185, 0x1D18B), (0x1D1AA, 0x1D1AD), (0x1D242, 0x1D244), (0x1DA00, 0x1DA36),
    (0x1DA3B, 0x1DA6C), (0x1DA75, 0x1DA75), (0x1DA84, 0x1DA84), (0x1DA9B, 0x1DA9F),
    (0x1DAA1, 0x1DAAF), (0xE0100, 0xE01EF),
];

#[derive(Clone, Copy)]
pub struct Cell {
    pub ch: char,
    pub fg: u32,
    pub bg: u32,
    pub attrs: u8,
}

impl Cell {
    fn blank(fg: u32, bg: u32) -> Cell {
        Cell { ch: ' ', fg, bg, attrs: 0 }
    }
}

/// The SGR pen: the fg/bg/attrs state one `CSI ... m` sequence mutates.
/// Extracted from the grid interpreter at H-2a+1 so halcyond's transcript
/// drives the SAME machinery per block (HALCYON.md §13.4(b) -- one SGR
/// implementation, two consumers; the grid's `sgr()` is now a thin shim
/// over this). Behavior notes that are load-bearing and easy to lose:
/// BOLD promotes a base-tier ANSI FG to the bright tier at application
/// time (so `1;31` and `31;1` differ exactly as before); bg never
/// promotes; truecolor passes through untouched; an empty parameter list
/// is the full reset (`CSI m`).
#[derive(Clone, Copy)]
pub struct SgrPen {
    pub fg: u32,
    pub bg: u32,
    pub attrs: u8,
}

impl SgrPen {
    pub fn new(pal: &Palette) -> SgrPen {
        SgrPen { fg: pal.fg, bg: pal.bg, attrs: 0 }
    }

    /// Apply one SGR parameter list (the numbers between `CSI` and `m`).
    pub fn apply(&mut self, pal: &Palette, params: &[u32]) {
        if params.is_empty() {
            self.fg = pal.fg;
            self.bg = pal.bg;
            self.attrs = 0;
            return;
        }
        let mut i = 0;
        while i < params.len() {
            let v = params[i];
            match v {
                0 => {
                    self.fg = pal.fg;
                    self.bg = pal.bg;
                    self.attrs = 0;
                }
                1 => self.attrs |= ATTR_BOLD,
                2 => self.attrs |= ATTR_DIM,
                3 => self.attrs |= ATTR_ITALIC,
                4 => self.attrs |= ATTR_UNDERLINE,
                5 | 6 => self.attrs |= ATTR_BLINK, // 6 (rapid) folded into blink
                7 => self.attrs |= ATTR_REVERSE,
                9 => self.attrs |= ATTR_STRIKE,
                22 => self.attrs &= !(ATTR_BOLD | ATTR_DIM), // 22 clears both
                23 => self.attrs &= !ATTR_ITALIC,
                24 => self.attrs &= !ATTR_UNDERLINE,
                25 => self.attrs &= !ATTR_BLINK,
                27 => self.attrs &= !ATTR_REVERSE,
                29 => self.attrs &= !ATTR_STRIKE,
                30..=37 => self.fg = self.ansi_fg(pal, (v - 30) as usize),
                39 => self.fg = pal.fg,
                40..=47 => self.bg = pal.ansi[(v - 40) as usize],
                49 => self.bg = pal.bg,
                90..=97 => self.fg = pal.ansi[(v - 90 + 8) as usize],
                100..=107 => self.bg = pal.ansi[(v - 100 + 8) as usize],
                38 | 48 => {
                    let (color, used) = extended_color(pal, params, i);
                    if let Some(c) = color {
                        if v == 38 {
                            self.fg = c;
                        } else {
                            self.bg = c;
                        }
                    }
                    i += used;
                }
                _ => {}
            }
            i += 1;
        }
    }

    fn ansi_fg(&self, pal: &Palette, idx: usize) -> u32 {
        // BOLD promotes the base tier to the bright tier (the classic
        // bold-as-bright terminal convention; we bake one weight).
        if self.attrs & ATTR_BOLD != 0 {
            pal.ansi[idx + 8]
        } else {
            pal.ansi[idx]
        }
    }
}

/// SGR 38/48 extended color at params[i]: `;2;r;g;b` or `;5;n`.
/// Returns (color, extra params consumed).
fn extended_color(pal: &Palette, params: &[u32], i: usize) -> (Option<u32>, usize) {
    if i + 1 >= params.len() {
        return (None, 0);
    }
    match params[i + 1] {
        2 => {
            if i + 4 < params.len() {
                let r = params[i + 2].min(255);
                let g = params[i + 3].min(255);
                let b = params[i + 4].min(255);
                (Some(0xFF00_0000 | (r << 16) | (g << 8) | b), 4)
            } else {
                (None, params.len() - i - 1)
            }
        }
        5 => {
            if i + 2 < params.len() {
                (Some(xterm256(pal, params[i + 2] as u8)), 2)
            } else {
                (None, 1)
            }
        }
        _ => (None, 1),
    }
}

enum State {
    Ground,
    Esc,
    EscCharset, // ESC ( / ESC ) -- swallow one designator byte
    Csi,
    Osc,
    OscEsc, // saw ESC inside OSC (ST = ESC \)
}

const MAX_PARAMS: usize = 16;

// Append `n` in decimal (the CPR reply formatter; no core::fmt in the byte
// machine's hot path).
fn push_dec(out: &mut Vec<u8>, n: usize) {
    let mut buf = [0u8; 8];
    let mut i = buf.len();
    let mut v = n.max(1); // rows/cols are 1-based and nonzero
    while v > 0 {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
    }
    out.extend_from_slice(&buf[i..]);
}

pub struct Vt {
    pub cols: usize,
    pub rows: usize,
    pub pal: Palette, // the live theme (set_theme remaps; main reads pal.bg for fills)
    pub cells: Vec<Cell>,
    alt_cells: Vec<Cell>, // the inactive buffer (alt-screen swap)
    pub on_alt: bool,
    pub cx: usize,
    pub cy: usize,
    pub cursor_visible: bool,
    fg: u32,
    bg: u32,
    attrs: u8,
    state: State,
    params: [u32; MAX_PARAMS],
    nparams: usize,
    cur_param: u32,
    param_seen: bool,
    csi_priv: bool,
    saved: (usize, usize),
    // DECSTBM scroll region [scroll_top, scroll_bot], 0-based inclusive; default
    // full screen. LF at the bottom margin, RI at the top, and SU/SD scroll only
    // this band; xterm resets it to full on resize. aurora never sets DECSTBM, so
    // the full-screen default preserves its behavior (the shared-crate contract).
    scroll_top: usize,
    scroll_bot: usize,
    // DECOM (?6): when set, CUP/VPA rows are origin-relative + region-clamped and
    // the cursor cannot leave the band; default off (absolute).
    origin: bool,
    // DECAWM (?7). Default SET (autowrap on -- the VT default). Kaua's
    // Terminal::enter emits ?7l so it can paint the bottom-right cell
    // scroll-free; ignoring it made every last-cell paint arm the deferred
    // wrap, and the next glyph run line-fed at the bottom row -- a full
    // screen SCROLL per status repaint, each leaving a stale modeline
    // behind (the nora artifact cascade, #37).
    wrap: bool,
    // DECAWM's slot in the DECSC/DECRC saved-cursor state (DEC STD-070:
    // ESC 7 saves position AND autowrap; ESC 8 restores both), which the
    // alt-screen switch (1049 = implicit DECSC on enter / DECRC on leave)
    // also carries -- so a TUI's ?7l inside the alt screen cannot leak a
    // wrap-off main screen (xterm parity; the G-5 F5 close). CSI s/u
    // (SCOSC) stays position-only, deliberately.
    saved_wrap: bool,
    // Bytes the terminal must ANSWER (CPR etc.). vt.rs stays a pure byte
    // machine: the main loop drains this into the consfeed fd, exactly a
    // real terminal answering on the keyboard wire. Kaua's size handshake
    // (SAVE + park-far + [6n + RESTORE) needs the reply or every Kaua app
    // falls back to 80x24 inside the real grid (#37).
    pub reply: Vec<u8>,
    // cfg-2b: the in-band settings channel (AURORA-CONFIG.md section 3.2) --
    // `OSC 7770;aurora;<key>;<value>` (BEL or ST terminated) lands here as a
    // `key value` line for the main loop to apply (the same grammar as the
    // config file, so config::parse serves both transports; a `reset` verb
    // re-seeds from the system file). SESSION-SCOPED by scripture: the main
    // loop never persists an OSC-applied setting. The xterm dynamic-colors
    // threat model applies (any console writer can emit it): cosmetic-only,
    // non-persistent, user-recoverable via F10 -- the channel must NEVER
    // gain a persisting or authority-bearing key. Bounded: payload cap 256,
    // queue cap 16 (drop beyond); a non-7770 OSC (titles) is swallowed as
    // before.
    pub settings_req: Vec<String>,
    osc_buf: Vec<u8>,
    osc_over: bool,
    // UTF-8 assembly (the prompt glyphs are multi-byte).
    utf_acc: u32,
    utf_rem: u8,
    pub dirty: Vec<bool>, // per-row damage
}

impl Vt {
    pub fn new(cols: usize, rows: usize) -> Vt {
        let pal = BONFIRE;
        Vt {
            cols,
            rows,
            pal,
            cells: vec![Cell::blank(pal.fg, pal.bg); cols * rows],
            alt_cells: vec![Cell::blank(pal.fg, pal.bg); cols * rows],
            on_alt: false,
            cx: 0,
            cy: 0,
            cursor_visible: true,
            fg: pal.fg,
            bg: pal.bg,
            attrs: 0,
            state: State::Ground,
            params: [0; MAX_PARAMS],
            nparams: 0,
            cur_param: 0,
            param_seen: false,
            csi_priv: false,
            saved: (0, 0),
            scroll_top: 0,
            scroll_bot: rows - 1,
            origin: false,
            wrap: true,
            saved_wrap: true,
            reply: Vec::new(),
            settings_req: Vec::new(),
            osc_buf: Vec::new(),
            osc_over: false,
            utf_acc: 0,
            utf_rem: 0,
            dirty: vec![true; rows],
        }
    }

    /// #55 (AURORA.md section 4): the reweave grid resize. Content-preserving
    /// and CURSOR-ANCHORED on the active screen: on a row shrink the visible
    /// window slides down just enough to keep the cursor row (the prompt); on
    /// grow, blank rows append at the bottom. Columns crop right / grow blank.
    /// The INACTIVE (alt) buffer is top-left-cropped -- a fullscreen TUI on
    /// the alt screen repaints itself on its own tty:winch. No history
    /// reflow -- fbcon-grade. Every row is marked dirty.
    pub fn resize(&mut self, ncols: usize, nrows: usize) {
        if (ncols == self.cols && nrows == self.rows) || ncols == 0 || nrows == 0 {
            return;
        }
        let shift = if self.cy >= nrows { self.cy + 1 - nrows } else { 0 };
        let ccols = if self.cols < ncols { self.cols } else { ncols };
        let (pfg, pbg) = (self.pal.fg, self.pal.bg);
        let mut cells = vec![Cell::blank(pfg, pbg); ncols * nrows];
        for r in 0..nrows {
            let or = r + shift;
            if or >= self.rows {
                break;
            }
            for c in 0..ccols {
                cells[r * ncols + c] = self.cells[or * self.cols + c];
            }
        }
        let mut alt = vec![Cell::blank(pfg, pbg); ncols * nrows];
        let arows = if self.rows < nrows { self.rows } else { nrows };
        for r in 0..arows {
            for c in 0..ccols {
                alt[r * ncols + c] = self.alt_cells[r * self.cols + c];
            }
        }
        self.cells = cells;
        self.alt_cells = alt;
        self.cols = ncols;
        self.rows = nrows;
        // DECSTBM resets to the full screen on resize (xterm parity).
        self.scroll_top = 0;
        self.scroll_bot = nrows - 1;
        self.cy = self.cy.saturating_sub(shift);
        if self.cy >= nrows {
            self.cy = nrows - 1;
        }
        if self.cx >= ncols {
            self.cx = ncols - 1;
        }
        self.saved = (
            if self.saved.0 >= ncols { ncols - 1 } else { self.saved.0 },
            if self.saved.1 >= nrows { nrows - 1 } else { self.saved.1 },
        );
        self.dirty = vec![true; nrows];
    }

    #[inline]
    fn mark(&mut self, row: usize) {
        if row < self.rows {
            self.dirty[row] = true;
        }
    }

    fn mark_all(&mut self) {
        for d in self.dirty.iter_mut() {
            *d = true;
        }
    }

    pub fn feed(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.byte(b);
        }
    }

    fn byte(&mut self, b: u8) {
        match self.state {
            State::Ground => self.ground(b),
            State::Esc => self.esc(b),
            State::EscCharset => self.state = State::Ground,
            State::Csi => self.csi(b),
            State::Osc => {
                if b == 0x07 {
                    self.osc_end();
                    self.state = State::Ground;
                } else if b == 0x1B {
                    self.state = State::OscEsc;
                } else if self.osc_buf.len() < 256 {
                    self.osc_buf.push(b);
                } else {
                    self.osc_over = true; // oversize: discard at terminator
                }
            }
            State::OscEsc => {
                // ST is ESC \; anything else stays in the OSC swallow (the
                // stray ESC is dropped from the payload -- a valid 7770
                // payload never contains one).
                if b == b'\\' {
                    self.osc_end();
                    self.state = State::Ground;
                } else {
                    self.state = State::Osc;
                }
            }
        }
    }

    fn ground(&mut self, b: u8) {
        // UTF-8 continuation assembly first.
        if self.utf_rem > 0 {
            if b & 0xC0 == 0x80 {
                self.utf_acc = (self.utf_acc << 6) | (b & 0x3F) as u32;
                self.utf_rem -= 1;
                if self.utf_rem == 0 {
                    let ch = char::from_u32(self.utf_acc).unwrap_or('\u{FFFD}');
                    self.put_char(ch);
                }
                return;
            }
            // Malformed sequence: drop the accumulator, reprocess this byte.
            self.utf_rem = 0;
        }
        match b {
            0x1B => self.state = State::Esc,
            b'\n' => self.line_feed(),
            b'\r' => {
                self.cx = 0;
            }
            0x08 => {
                if self.cx > 0 {
                    self.cx -= 1;
                }
            }
            b'\t' => {
                let next = (self.cx / 8 + 1) * 8;
                self.cx = next.min(self.cols - 1);
            }
            0x07 => {} // BEL
            0x00..=0x06 | 0x0B..=0x0C | 0x0E..=0x1F | 0x7F => {} // other C0 + DEL: drop
            0x20..=0x7E => self.put_char(b as char),
            0xC0..=0xDF => {
                self.utf_acc = (b & 0x1F) as u32;
                self.utf_rem = 1;
            }
            0xE0..=0xEF => {
                self.utf_acc = (b & 0x0F) as u32;
                self.utf_rem = 2;
            }
            0xF0..=0xF7 => {
                self.utf_acc = (b & 0x07) as u32;
                self.utf_rem = 3;
            }
            _ => {} // stray continuation byte
        }
    }

    fn esc(&mut self, b: u8) {
        self.state = State::Ground;
        match b {
            b'[' => {
                self.nparams = 0;
                self.cur_param = 0;
                self.param_seen = false;
                self.csi_priv = false;
                self.state = State::Csi;
            }
            b']' => {
                self.osc_buf.clear();
                self.osc_over = false;
                self.state = State::Osc;
            }
            b'(' | b')' => self.state = State::EscCharset,
            b'7' => {
                // DECSC: position + autowrap (DEC STD-070; G-5 F5).
                self.saved = (self.cx, self.cy);
                self.saved_wrap = self.wrap;
            }
            b'8' => {
                self.cx = self.saved.0.min(self.cols - 1);
                self.cy = self.saved.1.min(self.rows - 1);
                self.wrap = self.saved_wrap;
            }
            b'D' => self.line_feed(),
            b'M' => {
                // Reverse index: up, scrolling the region down at the top margin.
                if self.cy == self.scroll_top {
                    self.scroll_down();
                } else if self.cy > 0 {
                    self.cy -= 1;
                }
            }
            b'c' => {
                // RIS full reset.
                self.fg = self.pal.fg;
                self.bg = self.pal.bg;
                self.attrs = 0;
                self.cx = 0;
                self.cy = 0;
                self.scroll_top = 0;
                self.scroll_bot = self.rows - 1;
                self.origin = false;
                self.cursor_visible = true;
                let (fg, bg) = (self.pal.fg, self.bg);
                for c in self.cells.iter_mut() {
                    *c = Cell::blank(fg, bg);
                }
                self.mark_all();
            }
            _ => {}
        }
    }

    fn csi(&mut self, b: u8) {
        match b {
            b'0'..=b'9' => {
                self.cur_param = self
                    .cur_param
                    .saturating_mul(10)
                    .saturating_add((b - b'0') as u32);
                self.param_seen = true;
            }
            b';' => {
                self.push_param();
            }
            b'?' => self.csi_priv = true,
            b' '..=b'/' => {} // intermediates: swallow
            b':' => {
                // Sub-parameter separator (SGR 38:2:: form) -- treat like ';'
                // (adequate for the tree's emitters, which use ';').
                self.push_param();
            }
            0x40..=0x7E => {
                self.push_param();
                self.dispatch_csi(b);
                self.state = State::Ground;
            }
            _ => self.state = State::Ground, // malformed: abort the sequence
        }
    }

    fn push_param(&mut self) {
        if self.nparams < MAX_PARAMS {
            self.params[self.nparams] = if self.param_seen { self.cur_param } else { 0 };
            self.nparams += 1;
        }
        self.cur_param = 0;
        self.param_seen = false;
    }

    fn p(&self, i: usize, default: u32) -> u32 {
        if i < self.nparams && self.params[i] != 0 {
            self.params[i]
        } else {
            default
        }
    }

    fn dispatch_csi(&mut self, fin: u8) {
        match fin {
            b'A' => self.cy = self.cy.saturating_sub(self.p(0, 1) as usize),
            b'B' => self.cy = (self.cy + self.p(0, 1) as usize).min(self.rows - 1),
            b'C' => self.cx = (self.cx + self.p(0, 1) as usize).min(self.cols - 1),
            b'D' => self.cx = self.cx.saturating_sub(self.p(0, 1) as usize),
            b'G' => self.cx = (self.p(0, 1) as usize - 1).min(self.cols - 1),
            b'd' => self.cy = self.origin_row(self.p(0, 1) as usize - 1),
            b'H' | b'f' => {
                self.cy = self.origin_row(self.p(0, 1) as usize - 1);
                self.cx = (self.p(1, 1) as usize - 1).min(self.cols - 1);
            }
            b'J' => self.erase_display(self.p(0, 0)),
            b'K' => self.erase_line(self.p(0, 0)),
            b'L' => self.insert_lines(self.p(0, 1) as usize),
            b'M' => self.delete_lines(self.p(0, 1) as usize),
            b'@' => self.insert_chars(self.p(0, 1) as usize),
            b'P' => self.delete_chars(self.p(0, 1) as usize),
            b'X' => self.erase_chars(self.p(0, 1) as usize),
            b'S' => self.scroll_region_up(self.p(0, 1) as usize),
            // CSI T with 0/1 params is SD; the 5-param form is xterm highlight
            // mouse tracking (unimplemented) -- don't read it as a giant scroll.
            b'T' if self.nparams <= 1 => self.scroll_region_down(self.p(0, 1) as usize),
            b'm' => self.sgr(),
            b'h' | b'l' => self.mode_set(fin == b'h'),
            b's' => self.saved = (self.cx, self.cy),
            b'u' => {
                self.cx = self.saved.0.min(self.cols - 1);
                self.cy = self.saved.1.min(self.rows - 1);
            }
            b'r' if !self.csi_priv => {
                // DECSTBM: set the scroll region [top, bot] (1-based params;
                // empty -> full screen). A malformed region (top >= bot, or out
                // of range) resets to full (xterm). Homes the cursor (origin-aware).
                let top = (self.p(0, 1).max(1) as usize) - 1;
                let bot = (self.p(1, self.rows as u32) as usize) - 1;
                if top < bot && bot < self.rows {
                    self.scroll_top = top;
                    self.scroll_bot = bot;
                } else {
                    self.scroll_top = 0;
                    self.scroll_bot = self.rows - 1;
                }
                self.cx = 0;
                self.cy = if self.origin { self.scroll_top } else { 0 };
            }
            // DSR. 6 = CPR: answer with the cursor position -- kaua's size
            // handshake (SAVE + park-far + [6n + RESTORE) reads the parked
            // report to learn the real grid; an unanswered request strands
            // every Kaua app at its 80x24 fallback. The reply goes out via
            // `reply` (the main loop writes it into the consfeed fd -- the
            // keyboard wire, like any terminal).
            b'n' if self.p(0, 0) == 6 => {
                let row = self.cy + 1;
                let col = self.cx.min(self.cols - 1) + 1;
                self.reply.extend_from_slice(b"\x1b[");
                push_dec(&mut self.reply, row);
                self.reply.push(b';');
                push_dec(&mut self.reply, col);
                self.reply.push(b'R');
            }
            _ => {}
        }
    }

    fn mode_set(&mut self, set: bool) {
        if !self.csi_priv {
            return; // ANSI modes (4 IRM etc.): not implemented
        }
        for i in 0..self.nparams {
            match self.params[i] {
                6 => {
                    // DECOM origin mode. Set/reset homes the cursor to the
                    // (origin-relative) top-left (xterm / DEC STD-070).
                    self.origin = set;
                    self.cx = 0;
                    self.cy = if set { self.scroll_top } else { 0 };
                }
                7 => self.wrap = set, // DECAWM (kaua paints the last cell under ?7l)
                25 => self.cursor_visible = set,
                47 | 1047 | 1049 => self.alt_screen(set),
                _ => {}
            }
        }
    }

    fn alt_screen(&mut self, enter: bool) {
        if enter == self.on_alt {
            return;
        }
        core::mem::swap(&mut self.cells, &mut self.alt_cells);
        self.on_alt = enter;
        if enter {
            // A fresh alt screen (1049 semantics: implicit DECSC, so
            // autowrap saves with the cursor): clear + home.
            let (fg, bg) = (self.pal.fg, self.bg);
            for c in self.cells.iter_mut() {
                *c = Cell::blank(fg, bg);
            }
            self.saved = (self.cx, self.cy);
            self.saved_wrap = self.wrap;
            self.cx = 0;
            self.cy = 0;
        } else {
            // Implicit DECRC: a TUI's ?7l inside the alt screen must not
            // leak a wrap-off main screen (G-5 F5).
            self.cx = self.saved.0.min(self.cols - 1);
            self.cy = self.saved.1.min(self.rows - 1);
            self.wrap = self.saved_wrap;
        }
        self.mark_all();
    }

    fn sgr(&mut self) {
        // The shim over the extracted SgrPen (one SGR machinery, two
        // consumers -- see SgrPen). Copy-in/apply/copy-out keeps the grid's
        // field layout untouched.
        let mut pen = SgrPen { fg: self.fg, bg: self.bg, attrs: self.attrs };
        pen.apply(&self.pal, &self.params[..self.nparams]);
        self.fg = pen.fg;
        self.bg = pen.bg;
        self.attrs = pen.attrs;
    }

    fn put_char(&mut self, ch: char) {
        let w = char_width(ch);
        if w == 0 {
            // Combining / zero-width: no column of its own, no advance. (A
            // fuller impl folds it into the preceding cell; either way the
            // grid stays width-correct -- the invariant is it takes no column.)
            return;
        }
        // Resolve a deferred wrap left pending by the previous glyph.
        if self.cx >= self.cols {
            if self.wrap {
                self.cx = 0;
                self.line_feed();
            } else {
                // DECAWM reset: the cursor sticks at the right margin; each
                // new glyph overwrites the last column (the VT100 rule).
                self.cx = self.cols - 1;
            }
        }
        // A double-width glyph never straddles the right margin: with autowrap
        // it moves whole to the next line; without, it degrades to a single-
        // width paint clamped into the last cell.
        if w == 2 && self.cx + 1 >= self.cols {
            if self.wrap {
                self.cx = 0;
                self.line_feed();
            } else {
                let (cx, cy) = (self.cols - 1, self.cy);
                self.write_cell(cx, cy, ch, self.attrs);
                self.cx = self.cols - 1;
                return;
            }
        }
        let (cx, cy) = (self.cx, self.cy);
        let attrs = if w == 2 { self.attrs | ATTR_WIDE } else { self.attrs };
        self.write_cell(cx, cy, ch, attrs);
        if w == 2 {
            // The continuation (right half): a blank carrying the pen (so
            // reverse video / bg covers both cells), NOT marked wide.
            self.write_cell(cx + 1, cy, ' ', self.attrs);
        }
        self.cx += w;
    }

    fn write_cell(&mut self, cx: usize, cy: usize, ch: char, attrs: u8) {
        let idx = cy * self.cols + cx;
        self.cells[idx] = Cell {
            ch,
            fg: if attrs & ATTR_BOLD != 0 && self.fg == self.pal.fg {
                // Bold default-fg stays fg (no brighter tier exists).
                self.pal.fg
            } else {
                self.fg
            },
            bg: self.bg,
            attrs,
        };
        self.mark(cy);
    }

    // Map a 0-based CUP/VPA row through DECOM: origin-relative + region-clamped
    // when set, absolute (clamped to the screen) when reset.
    fn origin_row(&self, row: usize) -> usize {
        if self.origin {
            (self.scroll_top + row).min(self.scroll_bot)
        } else {
            row.min(self.rows - 1)
        }
    }

    fn line_feed(&mut self) {
        if self.cy == self.scroll_bot {
            self.scroll_up();
        } else if self.cy + 1 < self.rows {
            self.cy += 1;
        }
    }

    fn scroll_up(&mut self) {
        let cols = self.cols;
        let (top, bot) = (self.scroll_top, self.scroll_bot);
        // Shift rows (top+1..=bot) up one within the band; blank row `bot`.
        self.cells.copy_within((top + 1) * cols..(bot + 1) * cols, top * cols);
        let (fg, bg) = (self.pal.fg, self.bg);
        for c in self.cells[bot * cols..(bot + 1) * cols].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        self.mark_all();
    }

    fn scroll_down(&mut self) {
        let cols = self.cols;
        let (top, bot) = (self.scroll_top, self.scroll_bot);
        // Shift rows (top..bot) down one within the band; blank row `top`.
        self.cells.copy_within(top * cols..bot * cols, (top + 1) * cols);
        let (fg, bg) = (self.pal.fg, self.bg);
        for c in self.cells[top * cols..(top + 1) * cols].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        self.mark_all();
    }

    // SU (CSI S) / SD (CSI T): scroll the band by n lines, cursor unmoved.
    // n is clamped to the band height -- scrolling by >= the region height
    // just clears it, and the clamp bounds the cost against a hostile count.
    fn scroll_region_up(&mut self, n: usize) {
        let band = self.scroll_bot - self.scroll_top + 1;
        for _ in 0..n.min(band) {
            self.scroll_up();
        }
    }

    fn scroll_region_down(&mut self, n: usize) {
        let band = self.scroll_bot - self.scroll_top + 1;
        for _ in 0..n.min(band) {
            self.scroll_down();
        }
    }

    fn erase_display(&mut self, mode: u32) {
        let (fg, bg) = (self.pal.fg, self.bg);
        // Clamp cx: put_char leaves the cursor in the deferred-wrap state
        // cx == cols (past the last column) after a line that exactly fills
        // the width. Mode 1's inclusive `..=cur` bound would then form
        // cur == cells.len() -> `[..=len]` is out of range (a panic ->
        // renderer abort, reachable from any console writer). The clamp
        // erases through the last column of the current row (the cursor is
        // logically ON the last column at the wrap point). Holotype G-4 F1.
        let cx = self.cx.min(self.cols - 1);
        let cur = self.cy * self.cols + cx;
        match mode {
            0 => {
                for c in self.cells[cur..].iter_mut() {
                    *c = Cell::blank(fg, bg);
                }
                for r in self.cy..self.rows {
                    self.mark(r);
                }
            }
            1 => {
                for c in self.cells[..=cur].iter_mut() {
                    *c = Cell::blank(fg, bg);
                }
                for r in 0..=self.cy {
                    self.mark(r);
                }
            }
            _ => {
                for c in self.cells.iter_mut() {
                    *c = Cell::blank(fg, bg);
                }
                self.mark_all();
            }
        }
    }

    fn erase_line(&mut self, mode: u32) {
        let (fg, bg) = (self.pal.fg, self.bg);
        let row = self.cy * self.cols;
        // Clamp cx (the deferred-wrap cx == cols state): mode 1's `cx + 1`
        // would form b == cols + 1 -> `cells[row .. row + cols + 1]` overruns
        // the row by one (the same F1 OOB as erase_display). Mode 0's `a = cx`
        // is already safe (an empty slice when cx == cols), but clamp it too
        // for consistency. Holotype G-4 F1.
        let cx = self.cx.min(self.cols - 1);
        let (a, b) = match mode {
            0 => (cx, self.cols),
            1 => (0, cx + 1),
            _ => (0, self.cols),
        };
        for c in self.cells[row + a..row + b].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        self.mark(self.cy);
    }

    fn insert_lines(&mut self, n: usize) {
        let n = n.min(self.rows - self.cy);
        if n == 0 {
            return;
        }
        let cols = self.cols;
        let start = self.cy * cols;
        let end = self.rows * cols;
        self.cells.copy_within(start..end - n * cols, start + n * cols);
        let (fg, bg) = (self.pal.fg, self.bg);
        for c in self.cells[start..start + n * cols].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        for r in self.cy..self.rows {
            self.mark(r);
        }
    }

    fn delete_lines(&mut self, n: usize) {
        let n = n.min(self.rows - self.cy);
        if n == 0 {
            return;
        }
        let cols = self.cols;
        let start = self.cy * cols;
        let end = self.rows * cols;
        self.cells.copy_within(start + n * cols..end, start);
        let (fg, bg) = (self.pal.fg, self.bg);
        for c in self.cells[end - n * cols..].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        for r in self.cy..self.rows {
            self.mark(r);
        }
    }

    fn insert_chars(&mut self, n: usize) {
        let n = n.min(self.cols - self.cx);
        if n == 0 {
            return;
        }
        let row = self.cy * self.cols;
        let (a, b) = (row + self.cx, row + self.cols);
        self.cells.copy_within(a..b - n, a + n);
        let (fg, bg) = (self.pal.fg, self.bg);
        for c in self.cells[a..a + n].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        self.mark(self.cy);
    }

    fn delete_chars(&mut self, n: usize) {
        let n = n.min(self.cols - self.cx);
        if n == 0 {
            return;
        }
        let row = self.cy * self.cols;
        let (a, b) = (row + self.cx, row + self.cols);
        self.cells.copy_within(a + n..b, a);
        let (fg, bg) = (self.pal.fg, self.bg);
        for c in self.cells[b - n..b].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        self.mark(self.cy);
    }

    fn erase_chars(&mut self, n: usize) {
        let n = n.min(self.cols - self.cx);
        let row = self.cy * self.cols;
        let (fg, bg) = (self.pal.fg, self.bg);
        for c in self.cells[row + self.cx..row + self.cx + n].iter_mut() {
            *c = Cell::blank(fg, bg);
        }
        self.mark(self.cy);
    }

    /// The OSC terminator: dispatch the buffered payload. Only the cfg-2b
    /// settings channel (`7770;aurora;<key>;<value>`) produces anything --
    /// every other OSC (titles etc.) is swallowed exactly as before. The
    /// key/value land as a `key value` line (the config-file grammar) so one
    /// fail-soft parser serves both transports.
    ///
    /// cfg-3 F1 (the OSC-laundering close): a CONTROL byte (< 0x20) in the
    /// key or value is REJECTED. This is the receiving-end twin of
    /// `aurora-push`'s own `b < 0x20` sender filter -- the raw
    /// `/dev/consdrain` channel carries every session byte, and WITHOUT this
    /// guard an embedded NEWLINE laundered a second statement past the
    /// single-token allowlist: `config::parse` re-splits its value on
    /// `.lines()`, so `theme spinifex\nmode 640 480` set the compositor
    /// tier (`mode`) from a session, which a later OSD save would then
    /// persist into the SYSTEM config + push through the gated startup path.
    /// A settings value is a single printable token; the `;` reject stays
    /// (no valid setting carries the field separator).
    fn osc_end(&mut self) {
        let over = self.osc_over;
        self.osc_over = false;
        if over {
            self.osc_buf.clear();
            return;
        }
        if let Ok(s) = core::str::from_utf8(&self.osc_buf) {
            if let Some(rest) = s.strip_prefix("7770;aurora;") {
                if let Some((k, v)) = rest.split_once(';') {
                    let clean = |t: &str| !t.is_empty() && !t.bytes().any(|b| b < 0x20 || b == b';');
                    if clean(k) && !v.contains(';') && !v.bytes().any(|b| b < 0x20)
                        && self.settings_req.len() < 16
                    {
                        let mut line = String::with_capacity(k.len() + 1 + v.len());
                        line.push_str(k);
                        line.push(' ');
                        line.push_str(v);
                        self.settings_req.push(line);
                    }
                }
            }
        }
        self.osc_buf.clear();
    }

    /// Switch the live theme (the OSD's Appearance/theme setting). Cells bake
    /// resolved colors at write time, so existing content retints by EXACT
    /// old->new color match across both screens + the current SGR state;
    /// truecolor (and any color no longer matching the old palette) passes
    /// through untouched, by design. Slot order matters where a palette
    /// aliases colors (Bonfire ansi[15] == fg): fg/bg win over the ansi scan,
    /// so aliased cells follow the default-fg role -- benign either way (a
    /// theme keeps ansi[15] ~= fg). Marks every row dirty; the caller owns
    /// the margins refill (pal.bg changed).
    pub fn set_theme(&mut self, idx: usize) {
        let new = THEMES[idx % THEMES.len()].1;
        let old = self.pal;
        if new == old {
            return;
        }
        let map = |c: u32| -> u32 {
            if c == old.fg {
                return new.fg;
            }
            if c == old.bg {
                return new.bg;
            }
            for i in 0..16 {
                if c == old.ansi[i] {
                    return new.ansi[i];
                }
            }
            c
        };
        for c in self.cells.iter_mut().chain(self.alt_cells.iter_mut()) {
            c.fg = map(c.fg);
            c.bg = map(c.bg);
        }
        self.fg = map(self.fg);
        self.bg = map(self.bg);
        self.pal = new;
        self.mark_all();
    }
}

/// xterm-256 palette: 0-15 the live ANSI map, 16-231 the 6x6x6 cube, 232-255
/// the grayscale ramp.
fn xterm256(pal: &Palette, n: u8) -> u32 {
    match n {
        0..=15 => pal.ansi[n as usize],
        16..=231 => {
            let v = n - 16;
            let steps = [0u32, 95, 135, 175, 215, 255];
            let r = steps[(v / 36) as usize];
            let g = steps[((v / 6) % 6) as usize];
            let b = steps[(v % 6) as usize];
            0xFF00_0000 | (r << 16) | (g << 8) | b
        }
        _ => {
            let g = 8 + 10 * (n - 232) as u32;
            0xFF00_0000 | (g << 16) | (g << 8) | g
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The VT parser is `no_std`+alloc but pure logic -- these host tests
    // (cargo test, std available) drive the byte stream directly. The atlas
    // blit (render.rs) needs a framebuffer + the baked atlas, so it stays
    // proven by the in-guest ls-gfx E2E; the parser is proven here.

    fn feed(vt: &mut Vt, s: &[u8]) {
        vt.feed(s);
    }

    // Holotype G-4 F1: the deferred-wrap-at-last-row + ESC[1J / ESC[1K case
    // that overran the cell grid (cx == cols -> the mode-1 inclusive bound
    // formed cells[..=len]). A pre-fix build PANICS here; post-fix it erases
    // cleanly. Reachable from any console writer, so this is the regression.
    #[test]
    fn erase_mode1_at_deferred_wrap_last_row_no_oob() {
        let mut vt = Vt::new(8, 4); // cols=8, rows=4
        // Move to the last row, then fill the row exactly to the width so the
        // cursor lands in the deferred-wrap state cx == cols.
        feed(&mut vt, b"\x1b[4;1H"); // CUP row 4 col 1 -> cy=3, cx=0
        feed(&mut vt, b"abcdefgh"); // 8 chars fill the row; cx now == cols (8)
        assert_eq!(vt.cy, 3);
        assert_eq!(vt.cx, vt.cols); // the deferred-wrap state
        // ESC[1J (erase from start of display to cursor inclusive) MUST NOT
        // panic. Pre-fix: cur = 3*8 + 8 = 32 == cells.len() -> cells[..=32] OOB.
        feed(&mut vt, b"\x1b[1J");
        // ESC[1K (erase from start of line to cursor inclusive) -- the sibling.
        feed(&mut vt, b"\x1b[4;1H");
        feed(&mut vt, b"abcdefgh");
        feed(&mut vt, b"\x1b[1K");
        // Survived without a panic; the grid is intact (the row was cleared).
        assert_eq!(vt.cells.len(), 32);
    }

    // A ~10-digit CSI numeric parameter: the accumulator's multiply
    // saturates but a plain `+ digit` add overflows u32 -> panic under the
    // shipped overflow-checks profile -> the console (aurora AND halcyond,
    // which share this scanner) dies. One untrusted escape sequence.
    #[test]
    fn csi_param_overflow_does_not_panic() {
        let mut vt = Vt::new(8, 2);
        feed(&mut vt, b"\x1b[9999999999mX"); // huge SGR param, then a printable
        assert_eq!(vt.cells[0].ch, 'X'); // absorbed; the text after survives
    }

    // The classic full-width type-past-the-edge deferred wrap: writing cols+1
    // printables wraps to the next row rather than OOB-ing.
    #[test]
    fn deferred_wrap_writes_next_row() {
        let mut vt = Vt::new(4, 3);
        feed(&mut vt, b"abcd"); // exactly fills row 0; cx == 4 (== cols)
        assert_eq!(vt.cy, 0);
        assert_eq!(vt.cx, vt.cols);
        feed(&mut vt, b"e"); // the wrap: cy -> 1, then write 'e' at (1,0)
        assert_eq!(vt.cy, 1);
        assert_eq!(vt.cx, 1);
        assert_eq!(vt.cells[0].ch, 'a');
        assert_eq!(vt.cells[4].ch, 'e'); // row 1, col 0
    }

    // A malformed / truncated escape sequence must never panic or desync
    // the ground state.
    #[test]
    fn malformed_escapes_do_not_panic() {
        let mut vt = Vt::new(10, 4);
        feed(&mut vt, b"\x1b[999;999H"); // way-out-of-range CUP -> clamped
        assert!(vt.cx < vt.cols && vt.cy < vt.rows);
        feed(&mut vt, b"\x1b["); // truncated CSI
        feed(&mut vt, b"\x1b[38;2;300;400;500m"); // out-of-range truecolor
        feed(&mut vt, b"\x1b]0;title-with-no-terminator"); // unterminated OSC
        feed(&mut vt, b"\xff\xfe"); // stray UTF-8 continuation bytes
        feed(&mut vt, b"x"); // ground state still works
        // No panic reaching here is the assertion.
    }

    // Alt-screen enter/leave swaps a fresh buffer and restores dims + cursor.
    #[test]
    fn alt_screen_swap_preserves_dims() {
        let mut vt = Vt::new(6, 3);
        feed(&mut vt, b"main");
        feed(&mut vt, b"\x1b[?1049h"); // enter alt: clear + home
        assert_eq!(vt.cx, 0);
        assert_eq!(vt.cells.len(), 18);
        feed(&mut vt, b"alt");
        feed(&mut vt, b"\x1b[?1049l"); // leave: restore
        assert_eq!(vt.cells.len(), 18);
        assert_eq!(vt.cells[0].ch, 'm'); // the main buffer is back
    }

    // G-5 F5: DECAWM rides the DECSC/DECRC saved-cursor state (DEC
    // STD-070), and the alt-screen switch is an implicit DECSC/DECRC --
    // so a TUI that sets ?7l inside the alt screen (every Kaua app) must
    // NOT leak a wrap-off main screen when it leaves, even if it never
    // emits ?7h itself (the crash-exit shape ut's RESTORE_SCREEN also
    // covers; this makes the terminal correct WITHOUT the backstop).
    #[test]
    fn alt_screen_leave_restores_autowrap() {
        let mut vt = Vt::new(6, 3);
        assert!(vt.wrap);
        feed(&mut vt, b"\x1b[?1049h\x1b[?7l"); // enter alt, wrap off
        assert!(!vt.wrap);
        feed(&mut vt, b"\x1b[?1049l"); // leave WITHOUT ?7h
        assert!(vt.wrap); // the implicit DECRC restored autowrap
        // The explicit DECSC/DECRC pair carries it too.
        feed(&mut vt, b"\x1b7\x1b[?7l");
        assert!(!vt.wrap);
        feed(&mut vt, b"\x1b8");
        assert!(vt.wrap);
    }

    // #37: DECAWM (?7l) must make the bottom-right cell paintable WITHOUT a
    // scroll. Kaua disables autowrap at enter precisely for this; the
    // pre-fix parser ignored ?7, so painting the last cell armed the
    // deferred wrap and the NEXT glyph line-fed at the bottom row -- a
    // whole-screen scroll per status repaint (the nora artifact cascade).
    #[test]
    fn autowrap_off_bottom_right_paint_does_not_scroll() {
        let mut vt = Vt::new(8, 4);
        feed(&mut vt, b"\x1b[1;1Htop"); // sentinel on row 0
        feed(&mut vt, b"\x1b[?7l"); // kaua's DISABLE_AUTOWRAP
        feed(&mut vt, b"\x1b[4;1Habcdefgh"); // the bottom row THROUGH the last cell
        feed(&mut vt, b"XY"); // more glyphs, no move between: must NOT scroll
        assert_eq!(vt.cells[0].ch, 't', "row 0 intact -- no scroll happened");
        assert_eq!(vt.cy, 3);
        assert_eq!(vt.cx, vt.cols, "sticks at the margin (pending state kept)");
        // The last cell holds the final overprint (VT100 no-autowrap rule).
        assert_eq!(vt.cells[3 * 8 + 7].ch, 'Y');
        // Re-enable: the same sequence wraps + scrolls again (the default).
        feed(&mut vt, b"\x1b[?7h");
        feed(&mut vt, b"\x1b[4;1Habcdefgh");
        feed(&mut vt, b"Z");
        assert_ne!(vt.cells[0].ch, 't', "wrap-on at the bottom row scrolls");
    }

    // cfg-2b: the OSC settings channel -- 7770;aurora payloads land as
    // `key value` lines (BEL and ST both terminate); titles and malformed
    // payloads are swallowed; oversize discards.
    #[test]
    fn osc_settings_channel() {
        let mut vt = Vt::new(10, 4);
        feed(&mut vt, b"\x1b]7770;aurora;theme;parchment\x07"); // BEL
        feed(&mut vt, b"\x1b]7770;aurora;cursor-blink;off\x1b\\"); // ST
        assert_eq!(vt.settings_req.len(), 2);
        assert_eq!(vt.settings_req[0], "theme parchment");
        assert_eq!(vt.settings_req[1], "cursor-blink off");
        vt.settings_req.clear();
        feed(&mut vt, b"\x1b]0;a window title\x07"); // a normal OSC: swallowed
        feed(&mut vt, b"\x1b]7770;aurora;noval\x07"); // no value: ignored
        feed(&mut vt, b"\x1b]7770;aurora;k;v;extra\x07"); // ';' in value: ignored
        assert!(vt.settings_req.is_empty());
        // cfg-3 F1: a CONTROL byte in the value is REJECTED -- an embedded
        // newline was the laundering vector (config::parse re-splits the
        // value on .lines(), so `theme spinifex\nmode 640 480` slipped the
        // compositor-tier `mode` past the single-token allowlist). The
        // whole crafted OSC is dropped; nothing reaches settings_req.
        feed(&mut vt, b"\x1b]7770;aurora;theme;spinifex\nmode 640 480\x07");
        feed(&mut vt, b"\x1b]7770;aurora;theme\x01;evil\x07"); // control in key
        feed(&mut vt, b"\x1b]7770;aurora;theme;a\rb\x07"); // CR in value
        assert!(vt.settings_req.is_empty(),
            "a control byte in an OSC settings key/value must be rejected");
        // Oversize discards; the parser stays in sync after it.
        let mut big = Vec::from(&b"\x1b]7770;aurora;theme;"[..]);
        big.extend(core::iter::repeat(b'x').take(300));
        big.push(0x07);
        feed(&mut vt, &big);
        assert!(vt.settings_req.is_empty());
        feed(&mut vt, b"ok"); // ground state intact
        assert_eq!(vt.cells[0].ch, 'o');
    }

    // The OSD theme switch: exact-match retint across both screens + the SGR
    // state; truecolor survives untouched; a round-trip restores the original
    // colors exactly (the remap is a bijection over the palette slots).
    #[test]
    fn set_theme_remaps_exact_and_spares_truecolor() {
        let mut vt = Vt::new(8, 2);
        feed(&mut vt, b"\x1b[31mrr"); // ansi red (slot 1)
        feed(&mut vt, b"\x1b[38;2;1;2;3mt"); // truecolor
        feed(&mut vt, b"\x1b[0mp"); // default fg
        let (red0, tc, fg0) = (vt.cells[0].fg, vt.cells[2].fg, vt.cells[3].fg);
        assert_eq!(red0, BONFIRE.ansi[1]);
        assert_eq!(fg0, BONFIRE.fg);
        vt.set_theme(1); // parchment
        assert_eq!(vt.cells[0].fg, THEMES[1].1.ansi[1], "ansi slot follows");
        assert_eq!(vt.cells[2].fg, tc, "truecolor untouched");
        assert_eq!(vt.cells[3].fg, THEMES[1].1.fg, "default fg follows");
        assert_eq!(vt.cells[4].bg, THEMES[1].1.bg, "blank bg follows");
        assert!(vt.dirty.iter().all(|d| *d), "theme switch dirties all rows");
        vt.set_theme(0); // round-trip
        assert_eq!(vt.cells[0].fg, red0);
        assert_eq!(vt.cells[3].fg, fg0);
    }

    // #37: DSR 6 (CPR) must be ANSWERED -- kaua's size handshake parks the
    // cursor far and reads the report; an unanswered request strands every
    // Kaua app at its 80x24 fallback inside the real grid.
    #[test]
    fn cpr_reports_the_parked_cursor() {
        let mut vt = Vt::new(128, 36);
        feed(&mut vt, b"\x1b[9999;9999H"); // park far: clamps to (36, 128)
        feed(&mut vt, b"\x1b[6n");
        assert_eq!(vt.reply.as_slice(), b"\x1b[36;128R");
        vt.reply.clear();
        feed(&mut vt, b"\x1b[2;5H\x1b[6n");
        assert_eq!(vt.reply.as_slice(), b"\x1b[2;5R");
    }

    // --- KT-1a-1: DECSTBM scroll regions + DECOM origin mode ---

    #[test]
    fn decstbm_confines_scroll() {
        let mut vt = Vt::new(4, 5);
        feed(&mut vt, b"\x1b[1;1H0\x1b[2;1H1\x1b[3;1H2\x1b[4;1H3\x1b[5;1H4");
        feed(&mut vt, b"\x1b[2;4r"); // region 1-based 2;4 -> 0-based [1,3], homes (0,0)
        feed(&mut vt, b"\x1b[4;1H\n"); // cursor to the bottom margin (row 3), LF scrolls the band
        assert_eq!(vt.cells[0].ch, '0'); // row 0: outside the region, untouched
        assert_eq!(vt.cells[16].ch, '4'); // row 4: outside, untouched
        assert_eq!(vt.cells[4].ch, '2'); // row 1 <- old row 2
        assert_eq!(vt.cells[8].ch, '3'); // row 2 <- old row 3
        assert_eq!(vt.cells[12].ch, ' '); // row 3 blanked
    }

    #[test]
    fn decstbm_malformed_resets_full() {
        let mut vt = Vt::new(2, 4);
        feed(&mut vt, b"\x1b[9;99r"); // top=8 out of range -> reset to full [0,3]
        feed(&mut vt, b"\x1b[1;1Ha\x1b[4;1H\n"); // 'a' at (0,0), LF at the last row -> full scroll
        assert_eq!(vt.cells[0].ch, ' '); // 'a' scrolled off the top (proves the region is full)
    }

    #[test]
    fn origin_mode_cup_is_region_relative() {
        let mut vt = Vt::new(4, 6);
        feed(&mut vt, b"\x1b[3;5r"); // region 0-based [2,4]
        feed(&mut vt, b"\x1b[?6h"); // DECOM on (homes to scroll_top = 2)
        feed(&mut vt, b"\x1b[1;1HX"); // origin CUP 1;1 -> row scroll_top(2)
        assert_eq!(vt.cells[8].ch, 'X');
        feed(&mut vt, b"\x1b[99;1HY"); // origin CUP row 99 -> clamped to scroll_bot(4)
        assert_eq!(vt.cells[16].ch, 'Y');
        feed(&mut vt, b"\x1b[?6l\x1b[1;1HZ"); // DECOM off -> absolute, homes (0,0)
        assert_eq!(vt.cells[0].ch, 'Z');
    }

    #[test]
    fn ri_scrolls_region_down_at_top() {
        let mut vt = Vt::new(4, 5);
        feed(&mut vt, b"\x1b[1;1H0\x1b[2;1H1\x1b[3;1H2\x1b[4;1H3\x1b[5;1H4");
        feed(&mut vt, b"\x1b[2;4r\x1b[2;1H"); // region [1,3]; cursor to the top margin (row 1)
        feed(&mut vt, b"\x1bM"); // RI at the top margin -> scroll the band down
        assert_eq!(vt.cells[0].ch, '0'); // outside, untouched
        assert_eq!(vt.cells[16].ch, '4'); // outside, untouched
        assert_eq!(vt.cells[4].ch, ' '); // row 1 blanked
        assert_eq!(vt.cells[8].ch, '1'); // row 2 <- old row 1
        assert_eq!(vt.cells[12].ch, '2'); // row 3 <- old row 2
    }

    #[test]
    fn full_screen_lf_scrolls_all_no_decstbm() {
        // Backward-compat with aurora: no DECSTBM -> the whole screen scrolls.
        let mut vt = Vt::new(4, 3);
        feed(&mut vt, b"\x1b[1;1H0\x1b[2;1H1\x1b[3;1H2\x1b[3;1H\n");
        assert_eq!(vt.cells[0].ch, '1'); // row 0 <- old row 1
        assert_eq!(vt.cells[4].ch, '2'); // row 1 <- old row 2
        assert_eq!(vt.cells[8].ch, ' '); // row 2 blanked
    }

    #[test]
    fn decstbm_resets_on_resize() {
        let mut vt = Vt::new(4, 5);
        feed(&mut vt, b"\x1b[2;4r"); // region [1,3]
        vt.resize(4, 6); // resize -> region resets to full [0,5]
        feed(&mut vt, b"\x1b[1;1Ha\x1b[6;1H\n"); // 'a' at (0,0), LF at the new last row
        assert_eq!(vt.cells[0].ch, ' '); // full-screen scroll -> 'a' gone
    }

    #[test]
    fn su_scrolls_region_up_cursor_unmoved() {
        let mut vt = Vt::new(4, 5);
        feed(&mut vt, b"\x1b[1;1H0\x1b[2;1H1\x1b[3;1H2\x1b[4;1H3\x1b[5;1H4");
        feed(&mut vt, b"\x1b[2;4r"); // region 0-based [1,3]; homes cursor to (0,0)
        feed(&mut vt, b"\x1b[3;2H"); // park the cursor at cy=2, cx=1
        feed(&mut vt, b"\x1b[S"); // SU default 1 -> band shifts up, row 3 blanks
        assert_eq!(vt.cells[0].ch, '0'); // outside band, untouched
        assert_eq!(vt.cells[4].ch, '2'); // row 1 <- old row 2
        assert_eq!(vt.cells[8].ch, '3'); // row 2 <- old row 3
        assert_eq!(vt.cells[12].ch, ' '); // row 3 (scroll_bot) blanked
        assert_eq!(vt.cells[16].ch, '4'); // outside band, untouched
        assert_eq!((vt.cy, vt.cx), (2, 1)); // SU never moves the cursor
    }

    #[test]
    fn sd_scrolls_region_down() {
        let mut vt = Vt::new(4, 5);
        feed(&mut vt, b"\x1b[1;1H0\x1b[2;1H1\x1b[3;1H2\x1b[4;1H3\x1b[5;1H4");
        feed(&mut vt, b"\x1b[2;4r"); // region [1,3]
        feed(&mut vt, b"\x1b[T"); // SD default 1 -> band shifts down, row 1 blanks
        assert_eq!(vt.cells[0].ch, '0'); // outside, untouched
        assert_eq!(vt.cells[4].ch, ' '); // row 1 (scroll_top) blanked
        assert_eq!(vt.cells[8].ch, '1'); // row 2 <- old row 1
        assert_eq!(vt.cells[12].ch, '2'); // row 3 <- old row 2
        assert_eq!(vt.cells[16].ch, '4'); // outside, untouched
    }

    #[test]
    fn su_huge_count_clears_band_no_hang() {
        // The clamp: a hostile CSI count scrolls at most the band height, which
        // clears the region -- and bounds the loop (no multi-billion iteration).
        let mut vt = Vt::new(4, 5);
        feed(&mut vt, b"\x1b[1;1H0\x1b[2;1H1\x1b[3;1H2\x1b[4;1H3\x1b[5;1H4");
        feed(&mut vt, b"\x1b[2;4r"); // region [1,3], height 3
        feed(&mut vt, b"\x1b[999999S"); // clamped to 3 -> whole band cleared
        assert_eq!(vt.cells[0].ch, '0'); // outside, untouched
        assert_eq!(vt.cells[4].ch, ' ');
        assert_eq!(vt.cells[8].ch, ' ');
        assert_eq!(vt.cells[12].ch, ' ');
        assert_eq!(vt.cells[16].ch, '4'); // outside, untouched
    }

    #[test]
    fn sd_five_param_is_highlight_not_scroll() {
        // CSI 1;2;3;4;5 T is xterm highlight mouse tracking (unimplemented),
        // NOT SD -- a 5-param T must not scroll the screen.
        let mut vt = Vt::new(4, 3);
        feed(&mut vt, b"\x1b[1;1HA"); // 'A' at (0,0)
        feed(&mut vt, b"\x1b[1;2;3;4;5T"); // 5 params -> ignored, no scroll
        assert_eq!(vt.cells[0].ch, 'A'); // unmoved
        assert_eq!(vt.cells[4].ch, ' '); // did NOT scroll down into row 1
    }

    #[test]
    fn combining_table_is_sorted() {
        // in_ranges' binary search relies on sorted, non-overlapping ranges;
        // this catches any transcription slip in COMBINING.
        let mut prev: Option<u32> = None;
        for &(a, b) in COMBINING {
            assert!(a <= b, "inverted range {:#x}..{:#x}", a, b);
            if let Some(pv) = prev {
                assert!(a > pv, "range {:#x} not strictly after {:#x}", a, pv);
            }
            prev = Some(b);
        }
    }

    #[test]
    fn char_width_representative_points() {
        assert_eq!(char_width('A'), 1);
        assert_eq!(char_width(' '), 1);
        assert_eq!(char_width('\u{00E9}'), 1); // precomposed 'e-acute' is narrow
        assert_eq!(char_width('\u{0301}'), 0); // combining acute
        assert_eq!(char_width('\u{FE0F}'), 0); // variation selector-16
        assert_eq!(char_width('\u{4E00}'), 2); // CJK ideograph
        assert_eq!(char_width('\u{FF21}'), 2); // fullwidth A
        assert_eq!(char_width('\u{1F600}'), 2); // emoji
        assert_eq!(char_width('\u{1100}'), 2); // Hangul Jamo leading
        assert_eq!(char_width('\u{303F}'), 1); // the Kuhn wide-range exclusion
    }

    #[test]
    fn wide_char_advances_two_and_marks_left() {
        let mut vt = Vt::new(6, 2);
        feed(&mut vt, b"\xe4\xb8\x80"); // U+4E00 CJK (wide)
        assert_eq!(vt.cells[0].ch, '\u{4E00}');
        assert!(vt.cells[0].attrs & ATTR_WIDE != 0); // left half marked wide
        assert_eq!(vt.cells[1].ch, ' '); // blank continuation
        assert_eq!(vt.cells[1].attrs & ATTR_WIDE, 0); // continuation not marked
        assert_eq!(vt.cx, 2); // advanced two columns
    }

    #[test]
    fn wide_char_at_right_margin_wraps_whole() {
        let mut vt = Vt::new(3, 2); // cols = 3
        feed(&mut vt, b"ab"); // cols 0,1; cx = 2 (the last column)
        feed(&mut vt, b"\xe4\xb8\x80"); // wide: can't fit at col 2 -> wrap
        assert_eq!(vt.cells[2].ch, ' '); // col 2 left blank
        assert_eq!(vt.cells[3].ch, '\u{4E00}'); // row 1 col 0
        assert!(vt.cells[3].attrs & ATTR_WIDE != 0);
        assert_eq!(vt.cells[4].ch, ' '); // continuation
        assert_eq!((vt.cy, vt.cx), (1, 2));
    }

    #[test]
    fn wide_char_no_wrap_at_margin_degrades_single() {
        let mut vt = Vt::new(3, 2);
        feed(&mut vt, b"\x1b[?7l"); // DECAWM off
        feed(&mut vt, b"ab"); // cx = 2 at the last column
        feed(&mut vt, b"\xe4\xb8\x80"); // wide, no room, no wrap -> single
        assert_eq!(vt.cells[2].ch, '\u{4E00}'); // placed at the margin
        assert_eq!(vt.cells[2].attrs & ATTR_WIDE, 0); // degraded, not marked wide
        assert_eq!((vt.cy, vt.cx), (0, 2)); // cursor sticks at the margin
        assert_eq!(vt.cells[5].ch, ' '); // nothing spilled onto row 1
    }

    #[test]
    fn combining_mark_takes_no_column() {
        let mut vt = Vt::new(4, 2);
        feed(&mut vt, b"e"); // 'e' at col 0; cx = 1
        feed(&mut vt, b"\xcc\x81"); // U+0301 combining acute -> width 0
        assert_eq!(vt.cells[0].ch, 'e');
        assert_eq!(vt.cx, 1); // no advance for the combining mark
        feed(&mut vt, b"x"); // lands at col 1, not col 2
        assert_eq!(vt.cells[1].ch, 'x');
    }

    #[test]
    fn sgr_italic_dim_blink_strike_tracked() {
        let mut vt = Vt::new(8, 1);
        feed(&mut vt, b"\x1b[3mi"); // italic
        feed(&mut vt, b"\x1b[2md"); // + dim
        feed(&mut vt, b"\x1b[5mb"); // + blink
        feed(&mut vt, b"\x1b[9ms"); // + strike
        assert!(vt.cells[0].attrs & ATTR_ITALIC != 0);
        assert_eq!(
            vt.cells[1].attrs & (ATTR_ITALIC | ATTR_DIM),
            ATTR_ITALIC | ATTR_DIM
        );
        assert!(vt.cells[2].attrs & ATTR_BLINK != 0);
        assert!(vt.cells[3].attrs & ATTR_STRIKE != 0);
        feed(&mut vt, b"\x1b[23m\x1b[25m\x1b[29mR"); // clear italic/blink/strike
        let a = vt.cells[4].attrs;
        assert_eq!(a & (ATTR_ITALIC | ATTR_BLINK | ATTR_STRIKE), 0);
        assert!(a & ATTR_DIM != 0); // dim survives -- only 0/22 clear it
    }

    #[test]
    fn sgr_22_clears_bold_and_dim() {
        let mut vt = Vt::new(4, 1);
        feed(&mut vt, b"\x1b[1m\x1b[2mA"); // bold + dim
        assert_eq!(
            vt.cells[0].attrs & (ATTR_BOLD | ATTR_DIM),
            ATTR_BOLD | ATTR_DIM
        );
        feed(&mut vt, b"\x1b[22mB"); // 22 clears BOTH
        assert_eq!(vt.cells[1].attrs & (ATTR_BOLD | ATTR_DIM), 0);
    }
}
