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
use kaua::widget::{Block, List, Span, StatusLine, Widget};

use crate::editor::{Editor, Mode};
use crate::theme;
use crate::wrap;

/// The text-region layout for `area`: `(gutter_w, text_width, text_height)`.
/// The single source of geometry shared by `render` and `Editor::scroll_to`
/// (they must agree on the text width or the wrapped cursor desyncs).
pub fn text_metrics(area: Rect, line_count: usize) -> (u16, u16, usize) {
    let parts = Layout::vertical(&[Constraint::Min(1), Constraint::Length(1)]).split(area);
    let text_area = parts[0];
    let gutter_w = digits(line_count).max(3) + 1;
    let tw = text_area.width.saturating_sub(gutter_w);
    let th = text_area.height as usize;
    (gutter_w, tw, th)
}

/// Render `ed` into `buf` over `area`; returns the cursor `(x, y)`.
pub fn render(ed: &Editor, area: Rect, buf: &mut Buffer) -> (u16, u16) {
    let parts = Layout::vertical(&[Constraint::Min(1), Constraint::Length(1)]).split(area);
    let text_area = parts[0];
    let status_area = parts.get(1).copied().unwrap_or(text_area);
    let (gutter_w, tw, th) = text_metrics(area, ed.text.line_count());

    fill(buf, text_area, theme::blank());

    if ed.wrap {
        render_wrapped(ed, text_area, gutter_w, tw, th, buf);
    } else {
        render_plain(ed, text_area, gutter_w, tw, th, buf);
    }

    render_status(ed, status_area, buf);

    // The palette overlays the text; its cursor sits on the selected entry.
    if ed.palette_sel().is_some() {
        return render_palette(ed, text_area, buf);
    }

    if ed.wrap {
        cursor_wrapped(ed, text_area, gutter_w, tw, th)
    } else {
        cursor(ed, text_area, status_area, gutter_w)
    }
}

/// One logical line per screen row, clipped at the right edge (no wrap).
fn render_plain(ed: &Editor, text_area: Rect, gutter_w: u16, tw: u16, th: usize, buf: &mut Buffer) {
    let total = ed.text.line_count();
    let num_w = gutter_w.saturating_sub(1);
    let tx = text_area.x + gutter_w;
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
}

/// Soft-wrap: each logical line occupies ceil(len/tw) visual rows. The gutter
/// number shows on a line's first visual row, blank on continuations.
fn render_wrapped(ed: &Editor, text_area: Rect, gutter_w: u16, tw: u16, th: usize, buf: &mut Buffer) {
    let total = ed.text.line_count();
    let num_w = gutter_w.saturating_sub(1);
    let tx = text_area.x + gutter_w;
    let sel = ed.selection();
    let mut pos = Some((ed.top, ed.top_sub));
    for r in 0..th {
        let y = text_area.y + r as u16;
        let (row, sub) = match pos {
            Some(p) if p.0 < total => p,
            _ => {
                buf.set_str(text_area.x, y, "~", theme::tilde());
                pos = None;
                continue;
            }
        };
        if sub == 0 {
            let num = format!("{:>w$} ", row + 1, w = num_w as usize);
            buf.set_str(text_area.x, y, &num, theme::gutter());
        }
        let line = ed.text.line(row);
        draw_text_slice(buf, tx, y, tx + tw, line, sub * tw as usize, tw as usize, theme::text());
        if let Some(span) = sel {
            paint_selection_window(buf, tx, y, tx + tw, line, row, sub, tw, span);
        }
        pos = wrap::forward(&ed.text, (row, sub), tw);
    }
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

/// Like `draw_text` but draws the visual window `[skip, skip+take)` characters
/// of `s` (one soft-wrap sub-row).
fn draw_text_slice(
    buf: &mut Buffer,
    x: u16,
    y: u16,
    max_x: u16,
    s: &str,
    skip: usize,
    take: usize,
    style: kaua::style::Style,
) {
    let mut cx = x;
    for ch in s.chars().skip(skip).take(take) {
        if cx >= max_x {
            break;
        }
        let glyph = if ch.is_control() { ' ' } else { ch };
        buf.set_cell(cx, y, Cell::new(glyph, style));
        cx = cx.saturating_add(1);
    }
}

/// Paint the selection within one wrapped sub-row's window `[sub*tw, sub*tw+tw)`
/// of line `row`.
fn paint_selection_window(
    buf: &mut Buffer,
    tx: u16,
    y: u16,
    max_x: u16,
    line: &str,
    row: usize,
    sub: usize,
    tw: u16,
    span: ((usize, usize), (usize, usize)),
) {
    let (lo, hi) = span;
    if row < lo.0 || row > hi.0 {
        return;
    }
    let len = line.chars().count();
    let sel_start = if row == lo.0 { lo.1 } else { 0 };
    let sel_end = if row == hi.0 { (hi.1 + 1).min(len) } else { len };
    let win_start = sub * tw as usize;
    let win_end = win_start + tw as usize;
    let a = sel_start.max(win_start);
    let b = sel_end.min(win_end);
    let mut cc = a;
    while cc < b {
        let x = tx.saturating_add((cc - win_start) as u16);
        if x >= max_x {
            break;
        }
        let glyph = buf.get(x, y).map(|c| c.symbol).unwrap_or(' ');
        buf.set_cell(x, y, Cell::new(glyph, theme::selection()));
        cc += 1;
    }
}

/// The on-screen cursor in soft-wrap mode: walk visual rows from the scroll
/// anchor to the cursor's visual position (bounded by the viewport height --
/// `scroll_to` guarantees the cursor is within it).
fn cursor_wrapped(ed: &Editor, text_area: Rect, gutter_w: u16, tw: u16, th: usize) -> (u16, u16) {
    let tx = text_area.x + gutter_w;
    let cur = wrap::cursor_visual(&ed.text, tw);
    let mut p = (ed.top, ed.top_sub);
    let mut sy = 0usize;
    let mut steps = 0;
    while p != cur && steps < th {
        match wrap::forward(&ed.text, p, tw) {
            Some(q) => {
                p = q;
                sy += 1;
                steps += 1;
            }
            None => break,
        }
    }
    let (_, col) = ed.text.cursor();
    let vcol = col.saturating_sub(cur.1 * tw as usize) as u16;
    let y = text_area
        .y
        .saturating_add(sy.min(th.saturating_sub(1)) as u16);
    let x = tx
        .saturating_add(vcol)
        .min(text_area.right().saturating_sub(1));
    (x, y)
}

/// Draw the `[Space]` command palette as a popup at the text area's bottom-left;
/// returns the cursor position on the selected entry.
fn render_palette(ed: &Editor, text_area: Rect, buf: &mut Buffer) -> (u16, u16) {
    let labels = ed.palette_labels();
    let sel = ed.palette_sel();
    let title = " Space ";
    let inner_w = labels
        .iter()
        .map(|s| s.chars().count())
        .max()
        .unwrap_or(0)
        .max(title.chars().count()) as u16;
    let box_w = (inner_w + 4).min(text_area.width.max(1)); // 2 border + 2 pad
    let box_h = (labels.len() as u16 + 2).min(text_area.height.max(1)); // 2 border
    let by = text_area.bottom().saturating_sub(box_h);
    let box_area = Rect::new(text_area.x, by, box_w, box_h);
    fill(buf, box_area, theme::palette_surface());
    let block = Block::new()
        .title(title)
        .borders(true)
        .border_style(theme::palette_border())
        .title_style(theme::palette_title());
    let inner = block.inner(box_area);
    block.render(box_area, buf);
    List::new(&labels)
        .select(sel)
        .style(theme::palette_surface())
        .selected_style(theme::palette_selected())
        .render(inner, buf);
    let cy = inner
        .y
        .saturating_add(sel.unwrap_or(0) as u16)
        .min(inner.bottom().saturating_sub(1));
    (inner.x, cy)
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
    let right = match ed.buffer_indicator() {
        Some(b) => format!("{} {}:{} ", b, r + 1, c + 1),
        None => format!("{}:{} ", r + 1, c + 1),
    };

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
        Mode::Normal | Mode::Palette { .. } => theme::EMBER,
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

    #[test]
    fn wrap_render_splits_a_long_line() {
        // area 20x5 -> text width 16 (gutter_w 4). A 20-char line wraps: chars
        // 0..16 on visual row 0, 16..20 on row 1 (a blank gutter).
        let mut ed = Editor::new(None, "abcdefghijklmnopqrst", false);
        ed.toggle_wrap();
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        assert_eq!(sym(&b, 2, 0), '1'); // gutter "  1 " on the first visual row
        assert_eq!(sym(&b, 4, 0), 'a'); // text after the 4-wide gutter
        assert_eq!(sym(&b, 19, 0), 'p'); // char 15 at the right edge (tw=16)
        assert_eq!(sym(&b, 2, 1), ' '); // continuation row: blank gutter
        assert_eq!(sym(&b, 4, 1), 'q'); // char 16 wrapped onto row 1
        assert_eq!(sym(&b, 7, 1), 't'); // char 19
    }

    #[test]
    fn palette_overlay_renders_a_highlighted_menu() {
        let big = Rect::new(0, 0, 40, 12);
        let mut ed = Editor::new(None, "x", false);
        ed.handle_key(KeyEvent::char(' ')); // open palette, sel 0
        let mut b = Buffer::empty(big);
        render(&ed, big, &mut b);
        // 6 entries -> box height 8, bottom-aligned (by=3), inner rows 4..9;
        // entry 0 "toggle soft-wrap" selected, entry 1 "next buffer" not.
        assert_eq!(sym(&b, 1, 4), 't');
        assert_eq!(b.get(1, 4).unwrap().style.bg, theme::EMBER); // selected bar
        assert_eq!(sym(&b, 1, 5), 'n'); // "next buffer"
        assert_ne!(b.get(1, 5).unwrap().style.bg, theme::EMBER);
    }
}
