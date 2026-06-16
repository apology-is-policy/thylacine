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
use crate::syntax::HlClass;
use crate::theme;
use crate::wrap;

/// The text-region layout for `area`: `(gutter_w, text_width, text_height)`.
/// The single source of geometry shared by `render` and `Editor::scroll_to`
/// (they must agree on the text width or the wrapped cursor desyncs).
pub fn text_metrics(area: Rect, line_count: usize, tabs: bool) -> (u16, u16, usize) {
    let body = body_area(area, tabs);
    let parts = Layout::vertical(&[Constraint::Min(1), Constraint::Length(1)]).split(body);
    let text_area = parts[0];
    let gutter_w = digits(line_count).max(3) + 1;
    let tw = text_area.width.saturating_sub(gutter_w);
    let th = text_area.height as usize;
    (gutter_w, tw, th)
}

/// The region below the optional top buffer-tab strip (the whole area when no
/// tabs). Shared by `render` and `text_metrics` so they agree on the geometry.
fn body_area(area: Rect, tabs: bool) -> Rect {
    if tabs {
        Layout::vertical(&[Constraint::Length(1), Constraint::Min(1)]).split(area)[1]
    } else {
        area
    }
}

/// Render `ed` into `buf` over `area`; returns the cursor `(x, y)`.
pub fn render(ed: &Editor, area: Rect, buf: &mut Buffer) -> (u16, u16) {
    let tabs = ed.show_tabs();
    let body = body_area(area, tabs);
    let parts = Layout::vertical(&[Constraint::Min(1), Constraint::Length(1)]).split(body);
    let text_area = parts[0];
    let status_area = parts.get(1).copied().unwrap_or(text_area);
    let (gutter_w, tw, th) = text_metrics(area, ed.text.line_count(), tabs);

    // The buffer-tab strip occupies the top row when more than one buffer is open.
    if tabs {
        let tab_area =
            Layout::vertical(&[Constraint::Length(1), Constraint::Min(1)]).split(area)[0];
        render_tabs(ed, tab_area, buf);
    }

    fill(buf, text_area, theme::blank());

    if ed.wrap {
        render_wrapped(ed, text_area, gutter_w, tw, th, buf);
    } else {
        render_plain(ed, text_area, gutter_w, tw, th, buf);
    }

    render_status(ed, status_area, buf);

    // A faint corner nudge toward the manual on the pristine initial buffer
    // (gated to Mode::Normal in `show_hint`, so no overlay is up to fight it).
    if ed.show_hint() {
        render_hint(text_area, buf);
    }

    // The menu + pickers overlay the text and own the cursor.
    if ed.menu_open() {
        return render_menu(ed, text_area, buf);
    }
    if let Some(sel) = ed.buffer_picker_sel() {
        return render_buffer_picker(ed, text_area, sel, buf);
    }
    if let Some((query, sel)) = ed.file_picker_state() {
        return render_file_picker(ed, text_area, query, sel, buf);
    }

    // The ':' command popup lists matching commands above the command line. It is
    // a non-cursor help overlay -- the cursor stays on the command line itself.
    if ed.command_buf().is_some() {
        render_command_popup(ed, text_area, buf);
    }

    // The `s` select prompt owns the cursor on the status row (no block cursor).
    if let Some(pat) = ed.split_buf() {
        let col = "select: ".chars().count() + pat.chars().count();
        let x = status_area
            .x
            .saturating_add(col as u16)
            .min(status_area.right().saturating_sub(1));
        return (x, status_area.y);
    }
    let cur = if ed.wrap {
        cursor_wrapped(ed, text_area, gutter_w, tw, th)
    } else {
        cursor(ed, text_area, status_area, gutter_w)
    };
    // The mode-coloured block cursor. The binary hides the real terminal cursor
    // in text modes (`block_cursor`), so this painted block IS the cursor; on the
    // command line the real cursor shows instead and no block is drawn.
    if ed.block_cursor() {
        let accent = mode_color(&ed.mode);
        let glyph = buf.get(cur.0, cur.1).map(|c| c.symbol).unwrap_or(' ');
        buf.set_cell(cur.0, cur.1, Cell::new(glyph, theme::cursor_block(accent)));
        // Extra carets (multi-cursor): a block at each non-primary head (plain
        // mode; in wrap they show as highlighted ranges only at v1).
        if !ed.wrap {
            for sel in ed.carets().iter().skip(1) {
                if let Some((cx, cy)) = caret_screen(ed, text_area, gutter_w, sel.head) {
                    let g = buf.get(cx, cy).map(|c| c.symbol).unwrap_or(' ');
                    buf.set_cell(cx, cy, Cell::new(g, theme::cursor_block(accent)));
                }
            }
        }
    }
    cur
}

/// Draw the top buffer-tab strip (Helix-style) over the slate background: each
/// open buffer as a ` name ` segment, the active one an ember chip.
fn render_tabs(ed: &Editor, area: Rect, buf: &mut Buffer) {
    if area.is_empty() {
        return;
    }
    fill(buf, area, theme::tab_strip());
    let mut x = area.x;
    for (name, active) in ed.buffer_tabs() {
        let style = if active {
            theme::tab_active()
        } else {
            theme::tab_inactive()
        };
        for ch in format!(" {} ", name).chars() {
            if x >= area.right() {
                return;
            }
            buf.set_cell(x, area.y, Cell::new(ch, style));
            x = x.saturating_add(1);
        }
        // A one-cell slate gap separates adjacent tabs.
        if x < area.right() {
            buf.set_cell(x, area.y, Cell::new(' ', theme::tab_strip()));
            x = x.saturating_add(1);
        }
    }
}

/// One logical line per screen row, clipped at the right edge (no wrap).
fn render_plain(ed: &Editor, text_area: Rect, gutter_w: u16, tw: u16, th: usize, buf: &mut Buffer) {
    let total = ed.text.line_count();
    let num_w = gutter_w.saturating_sub(1);
    let tx = text_area.x + gutter_w;
    let ranges = ed.selection_ranges();
    let cur_row = ed.text.cursor().0;
    let lang = ed.lang();
    for r in 0..th {
        let y = text_area.y + r as u16;
        let row = ed.top + r;
        if row >= total {
            buf.set_str(text_area.x, y, "~", theme::tilde());
            continue;
        }
        let on_cur = row == cur_row;
        let (txt_style, gut_style) = if on_cur {
            (theme::current_line(), theme::current_gutter())
        } else {
            (theme::text(), theme::gutter())
        };
        if on_cur {
            // Lift the whole row so the highlight extends past the line's end.
            for x in text_area.x..text_area.right() {
                buf.set_cell(x, y, Cell::new(' ', theme::current_line()));
            }
        }
        let num = format!("{:>w$} ", row + 1, w = num_w as usize);
        buf.set_str(text_area.x, y, &num, gut_style);
        // Horizontal scroll: draw the window [left, left+tw) of the line.
        let line = ed.text.line(row);
        let classes = lang.line_classes(line);
        draw_text_slice(buf, tx, y, tx + tw, line, ed.left, tw as usize, txt_style, &classes);
        for &span in &ranges {
            paint_selection(buf, tx, y, tx + tw, line, row, ed.left, span);
        }
    }
}

/// Soft-wrap: each logical line occupies ceil(len/tw) visual rows. The gutter
/// number shows on a line's first visual row, blank on continuations.
fn render_wrapped(ed: &Editor, text_area: Rect, gutter_w: u16, tw: u16, th: usize, buf: &mut Buffer) {
    let total = ed.text.line_count();
    let num_w = gutter_w.saturating_sub(1);
    let tx = text_area.x + gutter_w;
    let ranges = ed.selection_ranges();
    let cur_row = ed.text.cursor().0;
    let lang = ed.lang();
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
        let on_cur = row == cur_row;
        let (txt_style, gut_style) = if on_cur {
            (theme::current_line(), theme::current_gutter())
        } else {
            (theme::text(), theme::gutter())
        };
        if on_cur {
            for x in text_area.x..text_area.right() {
                buf.set_cell(x, y, Cell::new(' ', theme::current_line()));
            }
        }
        if sub == 0 {
            let num = format!("{:>w$} ", row + 1, w = num_w as usize);
            buf.set_str(text_area.x, y, &num, gut_style);
        }
        let line = ed.text.line(row);
        let classes = lang.line_classes(line);
        draw_text_slice(buf, tx, y, tx + tw, line, sub * tw as usize, tw as usize, txt_style, &classes);
        for &span in &ranges {
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
    left: usize,
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
        if cc < left {
            continue; // scrolled off the left edge
        }
        let x = tx.saturating_add((cc - left) as u16);
        if x >= max_x {
            break;
        }
        let glyph = buf.get(x, y).map(|c| c.symbol).unwrap_or(' ');
        buf.set_cell(x, y, Cell::new(glyph, theme::selection()));
    }
}

/// Like `draw_text` but draws the visual window `[skip, skip+take)` characters
/// of `s` (one soft-wrap sub-row), colouring each cell by its syntax class
/// (`classes`, indexed by absolute char position) over the `base` style's
/// background. `classes` shorter than the window paints the gap as `Text`.
fn draw_text_slice(
    buf: &mut Buffer,
    x: u16,
    y: u16,
    max_x: u16,
    s: &str,
    skip: usize,
    take: usize,
    base: kaua::style::Style,
    classes: &[HlClass],
) {
    let mut cx = x;
    for (j, ch) in s.chars().skip(skip).take(take).enumerate() {
        if cx >= max_x {
            break;
        }
        let glyph = if ch.is_control() { ' ' } else { ch };
        let class = classes.get(skip + j).copied().unwrap_or(HlClass::Text);
        buf.set_cell(cx, y, Cell::new(glyph, base.fg(theme::syntax(class))));
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

/// A bordered popup at the text area's bottom-left, sized to `content_w` x
/// `content_h` (plus border + padding), returning its inner content rect. Shared
/// by the menu + both pickers.
fn popup(text_area: Rect, title: &str, content_w: u16, content_h: u16, buf: &mut Buffer) -> Rect {
    let box_w = (content_w + 4).min(text_area.width.max(1)); // 2 border + 2 pad
    let box_h = (content_h + 2).min(text_area.height.max(1)); // 2 border
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
    inner
}

/// Widest of the given rows' char counts and the title (for sizing a popup).
fn widest(rows: &[&str], title: &str) -> u16 {
    rows.iter()
        .map(|s| s.chars().count())
        .max()
        .unwrap_or(0)
        .max(title.chars().count()) as u16
}

/// The `[Space]` which-key menu: one `key  description` row per entry, no
/// selection (it is keypress-driven). The cursor parks at the list's top-left.
fn render_menu(ed: &Editor, text_area: Rect, buf: &mut Buffer) -> (u16, u16) {
    let rows: alloc::vec::Vec<String> = ed
        .menu_entries()
        .iter()
        .map(|(k, l)| format!("{}  {}", k, l))
        .collect();
    let refs: alloc::vec::Vec<&str> = rows.iter().map(|s| s.as_str()).collect();
    let title = " <space> ";
    let inner = popup(text_area, title, widest(&refs, title), rows.len() as u16, buf);
    List::new(&refs).style(theme::palette_surface()).render(inner, buf);
    (inner.x, inner.y)
}

/// The buffer picker (Space-b): an arrow-selectable list of open buffers.
fn render_buffer_picker(ed: &Editor, text_area: Rect, sel: usize, buf: &mut Buffer) -> (u16, u16) {
    let rows: alloc::vec::Vec<String> = ed.buffer_tabs().into_iter().map(|(n, _)| n).collect();
    let refs: alloc::vec::Vec<&str> = rows.iter().map(|s| s.as_str()).collect();
    let title = " buffers ";
    let inner = popup(text_area, title, widest(&refs, title), rows.len() as u16, buf);
    List::new(&refs)
        .select(Some(sel))
        .style(theme::palette_surface())
        .selected_style(theme::palette_selected())
        .render(inner, buf);
    let cy = inner
        .y
        .saturating_add(sel as u16)
        .min(inner.bottom().saturating_sub(1));
    (inner.x, cy)
}

/// The fuzzy file picker (Space-f): a `> query` line above the filtered file
/// list. The cursor sits on the query line (you type to filter).
fn render_file_picker(
    ed: &Editor,
    text_area: Rect,
    query: &str,
    sel: usize,
    buf: &mut Buffer,
) -> (u16, u16) {
    let matches = ed.file_picker_filtered();
    let refs: alloc::vec::Vec<&str> = matches.iter().map(|s| s.as_str()).collect();
    let title = " open file ";
    let prompt = format!("> {}", query);
    let content_w = widest(&refs, title).max(prompt.chars().count() as u16).max(20);
    let content_h = (refs.len() as u16).saturating_add(1); // query row + list
    let inner = popup(text_area, title, content_w, content_h, buf);
    buf.set_str(inner.x, inner.y, &prompt, theme::status_msg());
    let list_area = Rect::new(
        inner.x,
        inner.y.saturating_add(1),
        inner.width,
        inner.height.saturating_sub(1),
    );
    List::new(&refs)
        .select(Some(sel))
        .style(theme::palette_surface())
        .selected_style(theme::palette_selected())
        .render(list_area, buf);
    let cx = inner
        .x
        .saturating_add(prompt.chars().count() as u16)
        .min(inner.right().saturating_sub(1));
    (cx, inner.y)
}

/// The `:` command popup (C2): the ex-commands matching what is typed, each with
/// its description, above the command line. A help overlay -- it does not own the
/// cursor (that stays on the command line where you keep typing).
fn render_command_popup(ed: &Editor, text_area: Rect, buf: &mut Buffer) {
    let comps = ed.command_completions();
    if comps.is_empty() {
        return;
    }
    let rows: alloc::vec::Vec<String> = comps
        .iter()
        .map(|(n, d)| format!(":{}  {}", n, d))
        .collect();
    let refs: alloc::vec::Vec<&str> = rows.iter().map(|s| s.as_str()).collect();
    let title = " commands ";
    let inner = popup(text_area, title, widest(&refs, title), rows.len() as u16, buf);
    List::new(&refs).style(theme::palette_surface()).render(inner, buf);
}

/// A faint right-aligned `hint: type :help` on the text area's last row, drawn
/// only for the pristine initial buffer (`Editor::show_hint`) to point new users
/// at the manual. Skipped if the area is too small to hold it clear of the
/// top-left cursor.
fn render_hint(text_area: Rect, buf: &mut Buffer) {
    let hint = "hint: type :help";
    let w = hint.chars().count() as u16;
    if text_area.height < 2 || text_area.width < w {
        return;
    }
    let y = text_area.bottom().saturating_sub(1);
    let x = text_area.right().saturating_sub(w);
    buf.set_str(x, y, hint, theme::hint());
}

/// Draw the status row: the command/search line in command mode, else the
/// mode chip + filename + position bar.
fn render_status(ed: &Editor, area: Rect, buf: &mut Buffer) {
    if area.is_empty() {
        return;
    }
    if let Some(pat) = ed.split_buf() {
        fill(buf, area, theme::cmdline());
        let line = format!("select: {}", pat);
        draw_text(buf, area.x, area.y, area.right(), &line, theme::cmdline());
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
        .saturating_add(col.saturating_sub(ed.left) as u16)
        .min(text_area.right().saturating_sub(1));
    (x, y)
}

/// Screen `(x, y)` for a buffer position `p` in plain (non-wrap) mode, or `None`
/// if it is scrolled out of `text_area`.
fn caret_screen(ed: &Editor, text_area: Rect, gutter_w: u16, p: (usize, usize)) -> Option<(u16, u16)> {
    if p.0 < ed.top || p.1 < ed.left {
        return None;
    }
    let y = text_area.y + (p.0 - ed.top) as u16;
    let x = text_area.x + gutter_w + (p.1 - ed.left) as u16;
    if y >= text_area.bottom() || x >= text_area.right() {
        return None;
    }
    Some((x, y))
}

fn mode_color(mode: &Mode) -> Color {
    match mode {
        Mode::Normal | Mode::Menu | Mode::BufferPicker { .. } | Mode::FilePicker { .. } => {
            theme::EMBER
        }
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
    fn space_menu_renders_which_key_rows() {
        // C1: Space opens a which-key popup of "key  description" rows, no
        // selection bar (it is keypress-driven, not arrow-navigated).
        let big = Rect::new(0, 0, 40, 14);
        let mut ed = Editor::new(None, "x", false);
        ed.handle_key(KeyEvent::char(' ')); // open the menu
        let mut b = Buffer::empty(big);
        render(&ed, big, &mut b);
        // 8 entries -> box height 10, bottom-aligned (by=3), inner.y=4; row 0 is
        // "f  open file picker".
        assert_eq!(sym(&b, 1, 4), 'f'); // the key
        assert_eq!(sym(&b, 4, 4), 'o'); // "open file picker" after "f  "
        assert_ne!(b.get(1, 4).unwrap().style.bg, theme::EMBER); // no selection bar
    }

    #[test]
    fn file_picker_renders_query_and_filtered_list() {
        // C3: Space-f shows a `> query` line above the fuzzy-filtered files.
        let big = Rect::new(0, 0, 40, 14);
        let mut ed = Editor::new(None, "x", false);
        ed.open_file_picker(alloc::vec![
            String::from("main.rs"),
            String::from("lib.rs"),
        ]);
        ed.handle_key(KeyEvent::char('l')); // filter -> "lib.rs" only
        let mut b = Buffer::empty(big);
        let cur = render(&ed, big, &mut b);
        // content_h = 1 (filtered) + 1 (query) = 2 -> box_h 4; by = 13-4 = 9;
        // inner.y = 10; the query row "> l" then the list row "lib.rs".
        assert_eq!(sym(&b, 1, 10), '>'); // prompt
        assert_eq!(sym(&b, 3, 10), 'l'); // the typed query char
        assert_eq!(sym(&b, 1, 11), 'l'); // "lib.rs" on the list row
        assert_eq!(cur, (4, 10)); // cursor after "> l" on the query line
    }

    #[test]
    fn horizontal_scroll_renders_window_and_shifts_cursor() {
        // A1: area 20x5 -> tw 16 (gutter 4). A 30-char line; cursor at col 20 ->
        // left = 20+1-16 = 5, so the first drawn char is index 5 and the cursor
        // sits at the right edge.
        let mut ed = Editor::new(None, "0123456789ABCDEFGHIJKLMNOPQRST", false);
        for _ in 0..20 {
            ed.text.move_right();
        }
        let a = Rect::new(0, 0, 20, 5);
        ed.scroll_to(16, 4);
        let mut b = Buffer::empty(a);
        let cur = render(&ed, a, &mut b);
        assert_eq!(ed.left, 5);
        assert_eq!(sym(&b, 4, 0), '5'); // line index 5 drawn first, after the gutter
        assert_eq!(cur.0, 19); // col 20 -> x = 4 + (20-5), clamped to the edge
    }

    #[test]
    fn current_line_is_highlighted() {
        // B1: the cursor's row carries the lifted surface background; other rows
        // keep the plain editor background.
        let ed = Editor::new(Some("f".into()), "hello\nworld", false); // cursor (0,0)
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        assert_eq!(b.get(5, 0).unwrap().style.bg, theme::BAR); // 'e', current line
        assert_eq!(b.get(4, 1).unwrap().style.bg, theme::BG); // 'w', other line
    }

    #[test]
    fn cursor_block_takes_the_mode_colour() {
        // B2: the cursor cell is a block in the active mode's accent (the glyph
        // is preserved); Normal = ember, Insert = moss.
        let mut ed = Editor::new(Some("f".into()), "hi", false);
        let mut b = Buffer::empty(area());
        let cur = render(&ed, area(), &mut b);
        assert_eq!(cur, (4, 0));
        assert_eq!(b.get(4, 0).unwrap().style.bg, theme::EMBER);
        assert_eq!(b.get(4, 0).unwrap().symbol, 'h');
        ed.handle_key(KeyEvent::char('i')); // -> Insert
        let mut b2 = Buffer::empty(area());
        render(&ed, area(), &mut b2);
        assert_eq!(b2.get(4, 0).unwrap().style.bg, theme::GREEN);
    }

    #[test]
    fn buffer_tabs_strip_renders_on_two_buffers() {
        // B3: a slate top strip with one segment per buffer; the active one is an
        // ember chip; the text region shifts down by the strip row.
        let big = Rect::new(0, 0, 40, 10);
        let mut ed = Editor::new(Some("alpha".into()), "x", false);
        ed.open_buffer(Some("beta".into()), "y"); // 2 buffers, active = beta (1)
        let mut b = Buffer::empty(big);
        render(&ed, big, &mut b);
        assert_eq!(b.get(0, 0).unwrap().style.bg, theme::SLATE); // strip / inactive tab
        assert_eq!(sym(&b, 1, 0), 'a'); // " alpha "
        assert_eq!(sym(&b, 9, 0), 'b'); // " beta " after the 7-cell tab + 1 gap
        assert_eq!(b.get(9, 0).unwrap().style.bg, theme::EMBER); // active = ember
        assert_eq!(sym(&b, 2, 1), '1'); // text region starts on row 1 now
    }

    #[test]
    fn command_popup_lists_matching_commands() {
        // C2: typing ":b" pops up the b-prefixed ex-commands above the command
        // line; the cursor stays on the command line itself.
        let big = Rect::new(0, 0, 50, 14);
        let mut ed = Editor::new(None, "x", false);
        ed.handle_key(KeyEvent::char(':'));
        ed.handle_key(KeyEvent::char('b')); // ":b" -> bn, bp, bd, bd!
        let mut b = Buffer::empty(big);
        let cur = render(&ed, big, &mut b);
        // 4 matches -> box_h 6; by = 13-6 = 7; inner.y = 8; row 0 = ":bn  ...".
        assert_eq!(sym(&b, 1, 8), ':');
        assert_eq!(sym(&b, 2, 8), 'b');
        assert_eq!(sym(&b, 3, 8), 'n');
        // the command line still shows ":b" on the status row and owns the cursor.
        assert_eq!(sym(&b, 0, 13), ':');
        assert_eq!(sym(&b, 1, 13), 'b');
        assert_eq!(cur, (2, 13)); // cursor after ":b" on the command line
    }

    #[test]
    fn ut_keyword_gets_syntax_colour() {
        // A `.ut` buffer highlights `if` as a keyword (Bonfire slate). The 'f'
        // (col 1) is not under the cursor (col 0), so it shows the syntax fg.
        let ed = Editor::new(Some("x.ut".into()), "if x", false);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        assert_eq!(sym(&b, 5, 0), 'f');
        assert_eq!(b.get(5, 0).unwrap().style.fg, theme::SLATE);
    }

    #[test]
    fn non_ut_buffer_is_not_highlighted() {
        // The same text in a non-`.ut` buffer keeps the body fg (no language).
        let ed = Editor::new(Some("plain".into()), "if x", false);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        assert_eq!(sym(&b, 5, 0), 'f');
        assert_eq!(b.get(5, 0).unwrap().style.fg, theme::FG);
    }

    #[test]
    fn multi_select_paints_all_matches() {
        // %, s, "b", Enter -> every 'b' (not the gap) carries the selection bg.
        let mut ed = Editor::new(None, "b b", false);
        ed.handle_key(KeyEvent::char('%'));
        ed.handle_key(KeyEvent::char('s'));
        ed.handle_key(KeyEvent::char('b'));
        ed.handle_key(KeyEvent::new(KeyCode::Enter));
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        // gutter 4: 'b'@4, ' '@5, 'b'@6.
        assert_eq!(b.get(6, 0).unwrap().style.bg, theme::VIOLET); // the 2nd match is selected
        assert_ne!(b.get(5, 0).unwrap().style.bg, theme::VIOLET); // the gap is not
    }

    #[test]
    fn hint_renders_on_a_pristine_buffer() {
        // The faint `hint: type :help` (16 chars) is right-aligned on the text
        // area's last row (y=3 in a 20x5 area): x = 20 - 16 = 4.
        let ed = Editor::new(None, "", false);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        assert_eq!(sym(&b, 4, 3), 'h');
        assert_eq!(sym(&b, 5, 3), 'i');
        assert_eq!(b.get(4, 3).unwrap().style.fg, theme::DIM);
    }

    #[test]
    fn hint_hidden_once_modified() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(KeyEvent::char('i'));
        ed.handle_key(KeyEvent::char('z'));
        ed.handle_key(KeyEvent::new(KeyCode::Esc)); // modified -> no hint
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        assert_ne!(sym(&b, 4, 3), 'h');
    }

    #[test]
    fn multi_carets_paint_blocks_during_insert() {
        // %, s "a", Enter, c, "X" -> "X X" with a caret block at each head.
        let mut ed = Editor::new(None, "a a", false);
        ed.handle_key(KeyEvent::char('%'));
        ed.handle_key(KeyEvent::char('s'));
        ed.handle_key(KeyEvent::char('a'));
        ed.handle_key(KeyEvent::new(KeyCode::Enter));
        ed.handle_key(KeyEvent::char('c'));
        ed.handle_key(KeyEvent::char('X'));
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        // "X X": carets at (0,1) and (0,3). The non-primary caret paints a moss
        // (Insert accent) block at col 3 -> x = gutter(4) + 3 = 7.
        assert_eq!(b.get(7, 0).unwrap().style.bg, theme::GREEN);
    }
}
