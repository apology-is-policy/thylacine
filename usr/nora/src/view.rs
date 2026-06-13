// nora::view -- render the editor into a kaua Buffer (pure, host-testable).
//
// `render` lays the screen out as a text region above a one-row status/command
// line, paints the gutter (line numbers), the visible text (control chars
// shown as a single space -- tab-to-tabstop expansion is a seam), the visual
// selection, and the status bar, then returns the on-screen cursor `(x, y)`
// the binary hands to `Terminal::set_cursor`. It reads kaua's pure layers only
// (Buffer / Layout / widgets / style) -- no terminal, no I/O.

use alloc::format;
use alloc::string::String;

use kaua::buffer::{Buffer, Cell};
use kaua::layout::{Constraint, Layout};
use kaua::rect::Rect;
use kaua::style::Color;
use kaua::widget::{Span, StatusLine, Widget};

use crate::editor::{Editor, Mode};
use crate::theme;

/// Render `ed` into `buf` over `area`; returns the cursor `(x, y)`.
pub fn render(ed: &Editor, area: Rect, buf: &mut Buffer) -> (u16, u16) {
    let parts = Layout::vertical(&[Constraint::Min(1), Constraint::Length(1)]).split(area);
    let text_area = parts[0];
    let status_area = parts.get(1).copied().unwrap_or(text_area);

    fill(buf, text_area, theme::blank());

    let total = ed.text.line_count();
    let num_w = digits(total).max(3);
    let gutter_w = num_w + 1;
    let tx = text_area.x + gutter_w;
    let tw = text_area.width.saturating_sub(gutter_w);
    let th = text_area.height as usize;
    let sel = ed.selection();

    for r in 0..th {
        let y = text_area.y + r as u16;
        let row = ed.top + r;
        if row >= total {
            buf.set_str(text_area.x, y, "~", theme::tilde());
            continue;
        }
        let num = format!("{:>w$} ", row + 1, w = num_w as usize);
        buf.set_str(text_area.x, y, &num, theme::gutter());
        draw_text(buf, tx, y, tx + tw, ed.text.line(row), theme::text());
        if let Some(span) = sel {
            paint_selection(buf, tx, y, tx + tw, ed.text.line(row), row, span);
        }
    }

    render_status(ed, status_area, buf);
    cursor(ed, text_area, status_area, gutter_w)
}

/// Fill `area` with `style` blanks.
fn fill(buf: &mut Buffer, area: Rect, style: kaua::style::Style) {
    for y in area.y..area.bottom() {
        for x in area.x..area.right() {
            buf.set_cell(x, y, Cell::new(' ', style));
        }
    }
}

/// Write `s` from `(x, y)` up to `max_x`, rendering any control char as a
/// single space (keeping the 1-cell-per-char model; the buffer retains the
/// real char, so it round-trips to disk).
fn draw_text(buf: &mut Buffer, x: u16, y: u16, max_x: u16, s: &str, style: kaua::style::Style) {
    let mut cx = x;
    for ch in s.chars() {
        if cx >= max_x {
            break;
        }
        let glyph = if ch.is_control() { ' ' } else { ch };
        buf.set_cell(cx, y, Cell::new(glyph, style));
        cx = cx.saturating_add(1);
    }
}

/// Re-paint the selected cells of line `row` (within `[lo, hi]` inclusive) with
/// the selection style, keeping each cell's glyph.
fn paint_selection(
    buf: &mut Buffer,
    tx: u16,
    y: u16,
    max_x: u16,
    line: &str,
    row: usize,
    span: ((usize, usize), (usize, usize)),
) {
    let (lo, hi) = span;
    if row < lo.0 || row > hi.0 {
        return;
    }
    let len = line.chars().count();
    let start = if row == lo.0 { lo.1 } else { 0 };
    // Inclusive of hi.1 on the last selected row; whole line otherwise.
    let end = if row == hi.0 {
        (hi.1 + 1).min(len)
    } else {
        len
    };
    for cc in start..end {
        let x = tx.saturating_add(cc as u16);
        if x >= max_x {
            break;
        }
        let glyph = buf.get(x, y).map(|c| c.symbol).unwrap_or(' ');
        buf.set_cell(x, y, Cell::new(glyph, theme::selection()));
    }
}

/// Draw the status row: the command/search line in command mode, else the
/// mode chip + filename + position bar.
fn render_status(ed: &Editor, area: Rect, buf: &mut Buffer) {
    if area.is_empty() {
        return;
    }
    if let Some(cmd) = ed.command_buf() {
        fill(buf, area, theme::cmdline());
        draw_text(buf, area.x, area.y, area.right(), cmd, theme::cmdline());
        return;
    }

    let mode = format!(" {} ", ed.mode_str());
    let name = ed.filename.as_deref().unwrap_or("[No Name]");
    let center = match &ed.status {
        Some(m) => m.clone(),
        None => {
            let mut s = String::from(name);
            if ed.modified {
                s.push_str(" [+]");
            }
            s
        }
    };
    let (r, c) = ed.text.cursor();
    let right = format!("{}:{} ", r + 1, c + 1);

    StatusLine::new()
        .fill(theme::statusbar())
        .left(Span::styled(&mode, theme::mode_chip(mode_color(&ed.mode))))
        .center(Span::styled(&center, theme::status_msg()))
        .right(Span::styled(&right, theme::statusbar()))
        .render(area, buf);
}

/// The on-screen cursor: on the command line in command mode, else over the
/// text cell, clamped to the visible region.
fn cursor(ed: &Editor, text_area: Rect, status_area: Rect, gutter_w: u16) -> (u16, u16) {
    if let Some(cmd) = ed.command_buf() {
        let x = status_area
            .x
            .saturating_add(cmd.chars().count() as u16)
            .min(status_area.right().saturating_sub(1));
        return (x, status_area.y);
    }
    let (row, col) = ed.text.cursor();
    let y = text_area
        .y
        .saturating_add((row.saturating_sub(ed.top)) as u16)
        .min(text_area.bottom().saturating_sub(1));
    let x = text_area
        .x
        .saturating_add(gutter_w)
        .saturating_add(col as u16)
        .min(text_area.right().saturating_sub(1));
    (x, y)
}

fn mode_color(mode: &Mode) -> Color {
    match mode {
        Mode::Normal => theme::EMBER,
        Mode::Insert => theme::GREEN,
        Mode::Visual => theme::VIOLET,
        Mode::Command(_) => theme::GOLD,
    }
}

/// Decimal digit count of `n` (>= 1).
fn digits(n: usize) -> u16 {
    let mut n = n;
    let mut d = 1u16;
    while n >= 10 {
        n /= 10;
        d += 1;
    }
    d
}

#[cfg(test)]
mod tests {
    use super::*;
    use kaua::event::{KeyCode, KeyEvent};

    fn sym(b: &Buffer, x: u16, y: u16) -> char {
        b.get(x, y).unwrap().symbol
    }

    fn area() -> Rect {
        Rect::new(0, 0, 20, 5)
    }

    #[test]
    fn renders_gutter_text_and_cursor() {
        let ed = Editor::new(Some("f".into()), "hello\nworld", false);
        let mut b = Buffer::empty(area());
        let cur = render(&ed, area(), &mut b);
        // gutter "  1 " then "hello": num_w == 3, gutter_w == 4.
        assert_eq!(sym(&b, 2, 0), '1');
        assert_eq!(sym(&b, 4, 0), 'h');
        assert_eq!(sym(&b, 2, 1), '2');
        assert_eq!(sym(&b, 4, 1), 'w');
        // cursor at (0,0) -> text col 0 -> x = gutter_w (4), y = 0.
        assert_eq!(cur, (4, 0));
    }

    #[test]
    fn tilde_past_end_of_buffer() {
        let ed = Editor::new(None, "one", false);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        // line 0 has "1 one"; rows 1.. (text rows) are '~'.
        assert_eq!(sym(&b, 0, 1), '~');
    }

    #[test]
    fn status_bar_shows_mode_chip() {
        let ed = Editor::new(Some("f".into()), "x", false);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        // status row is the last row (y == 4): " NOR ...".
        assert_eq!(sym(&b, 1, 4), 'N');
        assert_eq!(sym(&b, 2, 4), 'O');
        assert_eq!(sym(&b, 3, 4), 'R');
        // chip bg is the ember accent.
        assert_eq!(b.get(1, 4).unwrap().style.bg, theme::EMBER);
    }

    #[test]
    fn command_line_drawn_on_status_row() {
        let mut ed = Editor::new(None, "x", false);
        ed.handle_key(KeyEvent::char(':'));
        ed.handle_key(KeyEvent::char('w'));
        let mut b = Buffer::empty(area());
        let cur = render(&ed, area(), &mut b);
        assert_eq!(sym(&b, 0, 4), ':');
        assert_eq!(sym(&b, 1, 4), 'w');
        // cursor sits just past ":w" on the command row.
        assert_eq!(cur, (2, 4));
    }

    #[test]
    fn control_chars_render_as_space() {
        let ed = Editor::new(None, "a\tb", false);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        // "a", tab->space, "b" after the 4-wide gutter.
        assert_eq!(sym(&b, 4, 0), 'a');
        assert_eq!(sym(&b, 5, 0), ' ');
        assert_eq!(sym(&b, 6, 0), 'b');
    }

    #[test]
    fn selection_paints_violet() {
        let mut ed = Editor::new(None, "abcd", false);
        ed.handle_key(KeyEvent::char('v'));
        ed.handle_key(KeyEvent::new(KeyCode::Char('l'))); // select (0,0)..(0,1)
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        // text starts at x=4; selected cols 0,1 -> x 4,5 carry the selection bg.
        assert_eq!(b.get(4, 0).unwrap().style.bg, theme::VIOLET);
        assert_eq!(b.get(5, 0).unwrap().style.bg, theme::VIOLET);
    }

    #[test]
    fn digit_widths() {
        assert_eq!(digits(1), 1);
        assert_eq!(digits(9), 1);
        assert_eq!(digits(10), 2);
        assert_eq!(digits(999), 3);
        assert_eq!(digits(1000), 4);
    }
}
