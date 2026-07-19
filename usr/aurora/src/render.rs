// The cell-grid blit: Cornucopia atlas glyphs alpha-blended fg-over-bg, with
// U+2500-259F (box drawing + block elements) drawn PROCEDURALLY for
// pixel-perfect cell joins (the atlas deliberately omits them -- see
// usr/lib/cornucopia). The renderer touches only the caller's pixel slice;
// damage granularity is the row (the VT marks dirty rows; present rects span
// them).

use crate::vt::{Cell, Vt, ATTR_REVERSE, ATTR_UNDERLINE};
use cornucopia::Atlas;

pub struct Metrics {
    pub cell_w: usize,
    pub cell_h: usize,
    pub baseline: usize,
    pub off_x: usize, // centering margins (the grid rarely tiles the mode exactly)
    pub off_y: usize,
}

#[inline]
fn blend(bg: u32, fg: u32, a: u8) -> u32 {
    if a == 0 {
        return bg;
    }
    if a == 255 {
        return fg;
    }
    // The packed R|B lane trick is only lane-safe with na = 256-a and >>8:
    // each 16-bit lane's sum is then <= 255*256 = 0xFF00 and the shift moves
    // whole lanes (the R lane's shifted-in low bits land in the masked-out G
    // position). The pre-fix /255 form divided the PACKED word -- integer
    // division does not distribute over lanes (65536 == 1 mod 255), so the B
    // output absorbed R_sum*257's low byte: interiors (a==0/255 short-circuit)
    // stayed exact while every antialiased EDGE pixel got a garbage B
    // correlated with R -- thin glyphs (the prompt) read wholesale violet,
    // warm colors fringed violet/gold (the user's "something with the
    // oranges/yellows"). Measured-and-reproduced: fg white over Bonfire bg at
    // a=127 -> (120,116,6) and a=191 -> (174,168,239), the exact wild pixels
    // sampled from the screendump.
    let a = a as u32;
    let na = 256 - a;
    let rb = (((fg & 0x00FF_00FF) * a + (bg & 0x00FF_00FF) * na) >> 8) & 0x00FF_00FF;
    let g = (((fg & 0x0000_FF00) * a + (bg & 0x0000_FF00) * na) >> 8) & 0x0000_FF00;
    0xFF00_0000 | rb | g
}

/// Render rows [r0, r1) of the grid into the pixel buffer (stride = w).
/// `cursor` = Some((cx, cy)) draws the block cursor by inverting that cell.
pub fn render_rows(
    vt: &Vt,
    m: &Metrics,
    px: &mut [u32],
    w: usize,
    r0: usize,
    r1: usize,
    cursor: Option<(usize, usize)>,
) {
    for row in r0..r1.min(vt.rows) {
        for col in 0..vt.cols {
            let cell = vt.cells[row * vt.cols + col];
            let inverted = cursor == Some((col, row));
            draw_cell(&cell, m, px, w, col, row, inverted);
        }
    }
}

fn draw_cell(
    cell: &Cell,
    m: &Metrics,
    px: &mut [u32],
    w: usize,
    col: usize,
    row: usize,
    inverted: bool,
) {
    let (mut fg, mut bg) = (cell.fg, cell.bg);
    if cell.attrs & ATTR_REVERSE != 0 {
        core::mem::swap(&mut fg, &mut bg);
    }
    if inverted {
        core::mem::swap(&mut fg, &mut bg);
    }
    let x0 = m.off_x + col * m.cell_w;
    let y0 = m.off_y + row * m.cell_h;

    let cp = cell.ch as u32;
    if (0x2500..=0x259F).contains(&cp) {
        fill_cell(px, w, x0, y0, m, bg);
        draw_boxchar(cp, m, px, w, x0, y0, fg, bg);
    } else if cell.ch == ' ' {
        fill_cell(px, w, x0, y0, m, bg);
    } else if let Some(alpha) = Atlas::glyph(cell.ch) {
        for y in 0..m.cell_h {
            let dst = (y0 + y) * w + x0;
            let src = y * m.cell_w;
            for x in 0..m.cell_w {
                px[dst + x] = blend(bg, fg, alpha[src + x]);
            }
        }
    } else {
        // Unbaked codepoint: a hollow box (the classic notdef).
        fill_cell(px, w, x0, y0, m, bg);
        let (bx0, by0) = (x0 + 1, y0 + 3);
        let (bw, bh) = (m.cell_w - 2, m.cell_h.saturating_sub(8));
        for x in 0..bw {
            px[by0 * w + bx0 + x] = fg;
            px[(by0 + bh - 1) * w + bx0 + x] = fg;
        }
        for y in 0..bh {
            px[(by0 + y) * w + bx0] = fg;
            px[(by0 + y) * w + bx0 + bw - 1] = fg;
        }
    }
    if cell.attrs & ATTR_UNDERLINE != 0 {
        let uy = y0 + m.baseline + 1;
        if uy < y0 + m.cell_h {
            for x in 0..m.cell_w {
                px[uy * w + x0 + x] = fg;
            }
        }
    }
}

#[inline]
fn fill_cell(px: &mut [u32], w: usize, x0: usize, y0: usize, m: &Metrics, c: u32) {
    for y in 0..m.cell_h {
        let dst = (y0 + y) * w + x0;
        for x in 0..m.cell_w {
            px[dst + x] = c;
        }
    }
}

fn fill_rect(px: &mut [u32], w: usize, x0: usize, y0: usize, rw: usize, rh: usize, c: u32) {
    for y in 0..rh {
        let dst = (y0 + y) * w + x0;
        for x in 0..rw {
            px[dst + x] = c;
        }
    }
}

// Box-drawing decomposition: the light/heavy/double line-drawing set reduces
// to four half-arms (up/down/left/right) through the cell center; heavy and
// double render as light at this cell size (an MVP simplification -- the
// arm MASK is exact, so every join still connects). Block elements are
// direct fills; shades are alpha blends.
const ARM_U: u8 = 1;
const ARM_D: u8 = 2;
const ARM_L: u8 = 4;
const ARM_R: u8 = 8;

fn box_arms(cp: u32) -> u8 {
    match cp {
        0x2500 | 0x2501 | 0x254C | 0x254D | 0x2504 | 0x2505 | 0x2508 | 0x2509 => ARM_L | ARM_R,
        0x2502 | 0x2503 | 0x254E | 0x254F | 0x2506 | 0x2507 | 0x250A | 0x250B => ARM_U | ARM_D,
        0x250C..=0x250F => ARM_D | ARM_R,
        0x2510..=0x2513 => ARM_D | ARM_L,
        0x2514..=0x2517 => ARM_U | ARM_R,
        0x2518..=0x251B => ARM_U | ARM_L,
        0x251C..=0x2523 => ARM_U | ARM_D | ARM_R,
        0x2524..=0x252B => ARM_U | ARM_D | ARM_L,
        0x252C..=0x2533 => ARM_D | ARM_L | ARM_R,
        0x2534..=0x253B => ARM_U | ARM_L | ARM_R,
        0x253C..=0x254B => ARM_U | ARM_D | ARM_L | ARM_R,
        // Double-line set: same arm shapes.
        0x2550 => ARM_L | ARM_R,
        0x2551 => ARM_U | ARM_D,
        0x2552..=0x2554 => ARM_D | ARM_R,
        0x2555..=0x2557 => ARM_D | ARM_L,
        0x2558..=0x255A => ARM_U | ARM_R,
        0x255B..=0x255D => ARM_U | ARM_L,
        0x255E..=0x2560 => ARM_U | ARM_D | ARM_R,
        0x2561..=0x2563 => ARM_U | ARM_D | ARM_L,
        0x2564..=0x2566 => ARM_D | ARM_L | ARM_R,
        0x2567..=0x2569 => ARM_U | ARM_L | ARM_R,
        0x256A..=0x256C => ARM_U | ARM_D | ARM_L | ARM_R,
        // Rounded corners map to their square arms.
        0x256D => ARM_D | ARM_R,
        0x256E => ARM_D | ARM_L,
        0x256F => ARM_U | ARM_L,
        0x2570 => ARM_U | ARM_R,
        0x2571..=0x2573 => 0, // diagonals: unsupported (bg fill)
        0x2574 => ARM_L,
        0x2575 => ARM_U,
        0x2576 => ARM_R,
        0x2577 => ARM_D,
        0x2578 => ARM_L,
        0x2579 => ARM_U,
        0x257A => ARM_R,
        0x257B => ARM_D,
        0x257C => ARM_L | ARM_R,
        0x257D => ARM_U | ARM_D,
        0x257E => ARM_L | ARM_R,
        0x257F => ARM_U | ARM_D,
        _ => 0,
    }
}

fn draw_boxchar(cp: u32, m: &Metrics, px: &mut [u32], w: usize, x0: usize, y0: usize, fg: u32, bg: u32) {
    let (cw, ch) = (m.cell_w, m.cell_h);
    if (0x2580..=0x259F).contains(&cp) {
        // Block elements.
        match cp {
            0x2580 => fill_rect(px, w, x0, y0, cw, ch / 2, fg),           // upper half
            0x2581..=0x2588 => {
                // Lower eighths: 2581 = 1/8 ... 2588 = full.
                let e = (cp - 0x2580) as usize;
                let h = ch * e / 8;
                fill_rect(px, w, x0, y0 + ch - h, cw, h, fg);
            }
            0x2589..=0x258F => {
                // Left blocks: 2589 = 7/8 ... 258F = 1/8.
                let e = (0x2590 - cp) as usize;
                let bw = cw * e / 8;
                fill_rect(px, w, x0, y0, bw, ch, fg);
            }
            0x2590 => fill_rect(px, w, x0 + cw / 2, y0, cw - cw / 2, ch, fg), // right half
            0x2591 => shade(px, w, x0, y0, cw, ch, bg, fg, 64),
            0x2592 => shade(px, w, x0, y0, cw, ch, bg, fg, 128),
            0x2593 => shade(px, w, x0, y0, cw, ch, bg, fg, 192),
            0x2594 => fill_rect(px, w, x0, y0, cw, (ch + 7) / 8, fg),     // upper eighth
            0x2595 => fill_rect(px, w, x0 + cw - (cw + 7) / 8, y0, (cw + 7) / 8, ch, fg),
            0x2596 => fill_rect(px, w, x0, y0 + ch / 2, cw / 2, ch - ch / 2, fg),
            0x2597 => fill_rect(px, w, x0 + cw / 2, y0 + ch / 2, cw - cw / 2, ch - ch / 2, fg),
            0x2598 => fill_rect(px, w, x0, y0, cw / 2, ch / 2, fg),
            0x2599 => {
                fill_rect(px, w, x0, y0, cw / 2, ch, fg);
                fill_rect(px, w, x0, y0 + ch / 2, cw, ch - ch / 2, fg);
            }
            0x259A => {
                fill_rect(px, w, x0, y0, cw / 2, ch / 2, fg);
                fill_rect(px, w, x0 + cw / 2, y0 + ch / 2, cw - cw / 2, ch - ch / 2, fg);
            }
            0x259B => {
                fill_rect(px, w, x0, y0, cw, ch / 2, fg);
                fill_rect(px, w, x0, y0, cw / 2, ch, fg);
            }
            0x259C => {
                fill_rect(px, w, x0, y0, cw, ch / 2, fg);
                fill_rect(px, w, x0 + cw / 2, y0, cw - cw / 2, ch, fg);
            }
            0x259D => fill_rect(px, w, x0 + cw / 2, y0, cw - cw / 2, ch / 2, fg),
            0x259E => {
                fill_rect(px, w, x0 + cw / 2, y0, cw - cw / 2, ch / 2, fg);
                fill_rect(px, w, x0, y0 + ch / 2, cw / 2, ch - ch / 2, fg);
            }
            0x259F => {
                fill_rect(px, w, x0 + cw / 2, y0, cw - cw / 2, ch, fg);
                fill_rect(px, w, x0, y0 + ch / 2, cw, ch - ch / 2, fg);
            }
            _ => {}
        }
        return;
    }
    let arms = box_arms(cp);
    if arms == 0 {
        return;
    }
    // Line thickness 1px at cell_w 10; the center lines sit at the cell
    // midpoint so adjacent cells' arms join exactly.
    let (mx, my) = (cw / 2, ch / 2);
    if arms & ARM_L != 0 {
        fill_rect(px, w, x0, y0 + my, mx + 1, 1, fg);
    }
    if arms & ARM_R != 0 {
        fill_rect(px, w, x0 + mx, y0 + my, cw - mx, 1, fg);
    }
    if arms & ARM_U != 0 {
        fill_rect(px, w, x0 + mx, y0, 1, my + 1, fg);
    }
    if arms & ARM_D != 0 {
        fill_rect(px, w, x0 + mx, y0 + my, 1, ch - my, fg);
    }
}

fn shade(px: &mut [u32], w: usize, x0: usize, y0: usize, cw: usize, ch: usize, bg: u32, fg: u32, a: u8) {
    let c = blend(bg, fg, a);
    fill_rect(px, w, x0, y0, cw, ch, c);
}

// DORMANT host-harness tests (the G-4f named seam: aurora is no_std +
// aarch64, so `cargo test` needs the netd-style cfg_attr refactor; these
// document + pin the contract until then, alongside vt.rs's module).
#[cfg(test)]
mod tests {
    use super::blend;

    #[test]
    fn blend_lanes_do_not_cross() {
        // The packed-lane fix: fg white over the Bonfire bg. The pre-fix
        // /255 form divided the PACKED word (division does not distribute
        // over lanes; 65536 == 1 mod 255), contaminating B with R's lane:
        // (120,116,6) at a=127 and (174,168,239) at a=191 -- the exact wild
        // pixels sampled from the live screendump. The lane-safe 256-based
        // form must track the per-channel ideal within 1.
        let fg = 0xFF00_0000u32 | (228 << 16) | (221 << 8) | 216;
        let bg = 0xFF00_0000u32 | (14 << 16) | (12 << 8) | 12;
        for a in [1u8, 64, 127, 180, 191, 254] {
            let v = blend(bg, fg, a);
            let (r, g, b) = (v >> 16 & 255, v >> 8 & 255, v & 255);
            let ideal =
                |f: u32, bgc: u32| (f * a as u32 + bgc * (255 - a as u32)) / 255;
            assert!((r as i64 - ideal(228, 14) as i64).abs() <= 1);
            assert!((g as i64 - ideal(221, 12) as i64).abs() <= 1);
            assert!((b as i64 - ideal(216, 12) as i64).abs() <= 1);
        }
    }
}
