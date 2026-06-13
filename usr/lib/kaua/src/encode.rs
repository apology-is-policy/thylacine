// kaua::encode -- the diff -> VT/ANSI escape emission, and the screen-control
// escapes. Pure: everything writes into a `&mut Vec<u8>`, so the wire output is
// host-testable byte-for-byte without a terminal.
//
// The Terminal (kaua::term) drives this: it walks the back-vs-front diff and,
// per changed cell, emits a cursor move (only when the pen is not already
// there), an SGR (only when the style changed), and the glyph -- one batched
// buffer flushed to fd 1 per frame.

use alloc::vec::Vec;

use crate::style::{Color, Style};

// Screen-control escapes (constants, so the Terminal composes them by name).
pub const ENTER_ALT_SCREEN: &[u8] = b"\x1b[?1049h";
pub const LEAVE_ALT_SCREEN: &[u8] = b"\x1b[?1049l";
pub const HIDE_CURSOR: &[u8] = b"\x1b[?25l";
pub const SHOW_CURSOR: &[u8] = b"\x1b[?25h";
pub const DISABLE_AUTOWRAP: &[u8] = b"\x1b[?7l";
pub const ENABLE_AUTOWRAP: &[u8] = b"\x1b[?7h";
pub const CLEAR_SCREEN: &[u8] = b"\x1b[2J";
pub const RESET_SGR: &[u8] = b"\x1b[0m";

/// Append a decimal `u16` (no `alloc::format!` -- a tiny manual encoder).
pub fn push_num(out: &mut Vec<u8>, mut n: u16) {
    if n == 0 {
        out.push(b'0');
        return;
    }
    let mut digits = [0u8; 5]; // u16::MAX = 65535 -> 5 digits
    let mut i = 0;
    while n > 0 {
        digits[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
    }
    while i > 0 {
        i -= 1;
        out.push(digits[i]);
    }
}

/// Move the cursor to cell `(x, y)` (0-based) -> `ESC[<y+1>;<x+1>H` (1-based).
/// `x+1`/`y+1` cannot overflow a u16 in practice (a console is far under 65535
/// cells/side); the `saturating_add` keeps it total regardless.
pub fn push_move_to(out: &mut Vec<u8>, x: u16, y: u16) {
    out.extend_from_slice(b"\x1b[");
    push_num(out, y.saturating_add(1));
    out.push(b';');
    push_num(out, x.saturating_add(1));
    out.push(b'H');
}

/// Emit an SGR that fully establishes `style`: always reset (`0`) first, then
/// the attributes, then fg, then bg -- so no prior state leaks. `Color::Reset`
/// maps to the terminal default (`39`/`49`); `Rgb` to truecolor
/// (`38;2;r;g;b` / `48;2;r;g;b`).
pub fn push_sgr(out: &mut Vec<u8>, style: Style) {
    out.extend_from_slice(b"\x1b[0"); // reset, then add params
    let a = style.attr;
    use crate::style::Attr;
    if a.contains(Attr::BOLD) {
        out.extend_from_slice(b";1");
    }
    if a.contains(Attr::DIM) {
        out.extend_from_slice(b";2");
    }
    if a.contains(Attr::ITALIC) {
        out.extend_from_slice(b";3");
    }
    if a.contains(Attr::UNDERLINE) {
        out.extend_from_slice(b";4");
    }
    if a.contains(Attr::REVERSE) {
        out.extend_from_slice(b";7");
    }
    match style.fg {
        Color::Reset => out.extend_from_slice(b";39"),
        Color::Rgb(r, g, b) => push_rgb(out, b";38;2;", r, g, b),
    }
    match style.bg {
        Color::Reset => out.extend_from_slice(b";49"),
        Color::Rgb(r, g, b) => push_rgb(out, b";48;2;", r, g, b),
    }
    out.push(b'm');
}

fn push_rgb(out: &mut Vec<u8>, prefix: &[u8], r: u8, g: u8, b: u8) {
    out.extend_from_slice(prefix);
    push_num(out, r as u16);
    out.push(b';');
    push_num(out, g as u16);
    out.push(b';');
    push_num(out, b as u16);
}

/// Append the UTF-8 bytes of one glyph.
pub fn push_char(out: &mut Vec<u8>, c: char) {
    let mut buf = [0u8; 4];
    out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
}

/// Render a sequence of changed cells into `out`. `cells` yields `(x, y, &Cell)`
/// in row-major order (the `Buffer::diff` order, or every cell on a full
/// repaint). Tracks a virtual pen so a cursor move is emitted only when the next
/// cell is not where the pen already sits (a contiguous run on one row needs one
/// move), and an SGR only when the style changes. `pen`/`last_style` are
/// threaded so a caller can split a frame across calls; pass fresh `None`s for a
/// whole frame.
pub fn render_cells<'a, I>(
    out: &mut Vec<u8>,
    cells: I,
    pen: &mut Option<(u16, u16)>,
    last_style: &mut Option<Style>,
) where
    I: IntoIterator<Item = (u16, u16, &'a crate::buffer::Cell)>,
{
    for (x, y, cell) in cells {
        if *pen != Some((x, y)) {
            push_move_to(out, x, y);
        }
        if *last_style != Some(cell.style) {
            push_sgr(out, cell.style);
            *last_style = Some(cell.style);
        }
        push_char(out, cell.symbol);
        // After writing a glyph the terminal advances one column (autowrap is
        // disabled by the Terminal, so the right-edge case never wraps; the next
        // cell -- a new row's start -- mismatches the pen and re-moves anyway).
        *pen = Some((x.saturating_add(1), y));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::buffer::Cell;
    use crate::style::{Attr, Color, Style};

    fn s(out: &[u8]) -> alloc::string::String {
        alloc::string::String::from_utf8(out.to_vec()).unwrap()
    }

    #[test]
    fn num_encoding() {
        let case = |n: u16| {
            let mut o = Vec::new();
            push_num(&mut o, n);
            s(&o)
        };
        assert_eq!(case(0), "0");
        assert_eq!(case(7), "7");
        assert_eq!(case(80), "80");
        assert_eq!(case(65535), "65535");
    }

    #[test]
    fn move_to_is_one_based() {
        let mut o = Vec::new();
        push_move_to(&mut o, 0, 0);
        assert_eq!(s(&o), "\x1b[1;1H");
        let mut o2 = Vec::new();
        push_move_to(&mut o2, 9, 23); // col 10, row 24
        assert_eq!(s(&o2), "\x1b[24;10H");
    }

    #[test]
    fn sgr_truecolor_and_attrs() {
        let mut o = Vec::new();
        let st = Style::new()
            .fg(Color::Rgb(224, 120, 64))
            .bg(Color::Reset)
            .attr(Attr::BOLD);
        push_sgr(&mut o, st);
        assert_eq!(s(&o), "\x1b[0;1;38;2;224;120;64;49m");
    }

    #[test]
    fn sgr_default_style_is_reset() {
        let mut o = Vec::new();
        push_sgr(&mut o, Style::new());
        assert_eq!(s(&o), "\x1b[0;39;49m");
    }

    #[test]
    fn char_utf8() {
        let mut o = Vec::new();
        push_char(&mut o, 'A');
        push_char(&mut o, 'é');
        assert_eq!(o, [b'A', 0xc3, 0xa9]);
    }

    #[test]
    fn render_contiguous_run_emits_one_move() {
        // Two adjacent cells on a row, same style -> one move, one SGR, two
        // glyphs.
        let st = Style::new().fg(Color::Rgb(1, 2, 3));
        let a = Cell::new('h', st);
        let b = Cell::new('i', st);
        let cells = [(2u16, 0u16, &a), (3u16, 0u16, &b)];
        let mut o = Vec::new();
        let mut pen = None;
        let mut ls = None;
        render_cells(
            &mut o,
            cells.iter().map(|&(x, y, c)| (x, y, c)),
            &mut pen,
            &mut ls,
        );
        assert_eq!(s(&o), "\x1b[1;3H\x1b[0;38;2;1;2;3;49mhi");
        assert_eq!(pen, Some((4, 0)));
    }

    #[test]
    fn render_non_contiguous_re_moves() {
        let st = Style::new();
        let a = Cell::new('x', st);
        let b = Cell::new('y', st);
        // (0,0) then (5,1): non-contiguous -> two moves; same style -> one SGR.
        let cells = [(0u16, 0u16, &a), (5u16, 1u16, &b)];
        let mut o = Vec::new();
        let mut pen = None;
        let mut ls = None;
        render_cells(
            &mut o,
            cells.iter().map(|&(x, y, c)| (x, y, c)),
            &mut pen,
            &mut ls,
        );
        assert_eq!(s(&o), "\x1b[1;1H\x1b[0;39;49mx\x1b[2;6Hy");
    }

    #[test]
    fn render_style_change_re_emits_sgr() {
        let red = Style::new().fg(Color::Rgb(255, 0, 0));
        let grn = Style::new().fg(Color::Rgb(0, 255, 0));
        let a = Cell::new('r', red);
        let b = Cell::new('g', grn);
        let cells = [(0u16, 0u16, &a), (1u16, 0u16, &b)];
        let mut o = Vec::new();
        let mut pen = None;
        let mut ls = None;
        render_cells(
            &mut o,
            cells.iter().map(|&(x, y, c)| (x, y, c)),
            &mut pen,
            &mut ls,
        );
        // one move, then SGR+glyph, then SGR+glyph (contiguous so no 2nd move).
        // Each SGR fully establishes the style: reset, fg, and the default bg
        // (`;49`).
        assert_eq!(
            s(&o),
            "\x1b[1;1H\x1b[0;38;2;255;0;0;49mr\x1b[0;38;2;0;255;0;49mg"
        );
    }
}
