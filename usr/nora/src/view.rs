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
use kaua::style::{Color, Style};
use kaua::widget::{
    flatten_tree, Block, List, Row, Scrollbar, Span, StatusLine, Table, Tabs, Tree, TreeItem,
    Widget,
};

use crate::debug::DebugView;
use crate::diag::Severity as DiagSeverity;
use crate::editor::{DashPane, Editor, Mode};
use crate::syntax::HlClass;
use crate::theme;
use crate::wrap;

/// Floor sidebar width (NORA-IDE-UX section 2.3): the three debug tiles.
const SIDEBAR_W: u16 = 28;
/// Floor Console height (a Tabs strip + a few scrollback rows).
const CONSOLE_H: u16 = 7;
/// Below this the dashboard would crush the editor -- fall back to full-width
/// (the debug data still reaches the status line + `:bt`).
const DASH_MIN_W: u16 = 50;
const DASH_MIN_H: u16 = 14;

/// Most rows a hover popup may occupy. A gopls hover on a documented symbol
/// can run to a screenful; the box exists to answer "what is this", not to
/// replace the editor, and it is bounded so it can never bury the code the
/// question was about.
const HOVER_ROWS: usize = 8;
/// Most candidates the completion popup shows at once (it scrolls).
const COMPLETION_ROWS: usize = 10;

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

/// The editor + optional debugger dashboard. When a debug session is live and
/// the terminal is roomy enough, the screen splits into `[editor | sidebar]`
/// over a full-width Console (NORA-IDE-UX section 2); otherwise the editor is
/// full-width (the existing behavior, byte-for-byte). Returns the cursor, which
/// stays in the editor at 8f-2a (per-pane cursors are 8f-2b).
pub fn render(ed: &Editor, area: Rect, buf: &mut Buffer) -> (u16, u16) {
    match dash_split(ed, area) {
        Some(d) => {
            if let (Some(dv), Some(sb)) = (ed.debug_view(), d.sidebar) {
                render_sidebar(ed, dv, sb, buf);
            }
            if let (Some(dv), Some(cs)) = (ed.debug_view(), d.console) {
                render_console(ed, dv, cs, buf);
            }
            render_editor(ed, d.editor, buf)
        }
        None => render_editor(ed, area, buf),
    }
}

/// The non-overlapping tiles of the dashboard split.
struct DashRects {
    editor: Rect,
    sidebar: Option<Rect>,
    console: Option<Rect>,
}

/// The dashboard split for `area`, or `None` when not debugging or the terminal
/// is too small (both mean "render the editor full-width"). The single source
/// of the split geometry, shared by `render` and `editor_area` so they agree.
fn dash_split(ed: &Editor, area: Rect) -> Option<DashRects> {
    if !ed.debugging() || area.width < DASH_MIN_W || area.height < DASH_MIN_H {
        return None;
    }
    // Bottom Console spans the full width; the main band holds editor + sidebar.
    let rows = Layout::vertical(&[Constraint::Min(1), Constraint::Length(CONSOLE_H)]).split(area);
    let main = rows[0];
    let console = rows.get(1).copied();
    let cols =
        Layout::horizontal(&[Constraint::Min(1), Constraint::Length(SIDEBAR_W)]).split(main);
    Some(DashRects {
        editor: cols[0],
        sidebar: cols.get(1).copied(),
        console,
    })
}

/// The editor's sub-rect for the current layout (== `area` when full-width).
/// The binary calls this for `text_metrics`/`scroll_to`, so the viewport width
/// it scrolls to matches the width `render` draws into (else the wrapped cursor
/// desyncs).
pub fn editor_area(ed: &Editor, area: Rect) -> Rect {
    dash_split(ed, area).map(|d| d.editor).unwrap_or(area)
}

/// A dashboard tile frame: fill the background, draw the border + title (ember
/// when focused), and return the inner content rect.
fn tile(area: Rect, title: &str, focused: bool, buf: &mut Buffer) -> Rect {
    fill(buf, area, theme::blank());
    let block = Block::new()
        .title(title)
        .borders(true)
        .border_style(theme::tile_border(focused))
        .title_style(theme::tile_title(focused));
    let inner = block.inner(area);
    block.render(area, buf);
    inner
}

/// The right sidebar: Variables / Call Stack / Goroutines, stacked in equal
/// thirds. Each tile highlights its selected row + shows a scrollbar when it
/// overflows, but only the FOCUSED tile draws the row cursor (§2.3).
fn render_sidebar(ed: &Editor, dv: &DebugView, area: Rect, buf: &mut Buffer) {
    let d = ed.dash();
    let focus = d.focus;
    let tiles =
        Layout::vertical(&[Constraint::Fill, Constraint::Fill, Constraint::Fill]).split(area);
    render_variables(
        dv,
        tiles[0],
        focus == DashPane::Variables,
        d.var_sel,
        d.locals_expanded,
        buf,
    );
    if let Some(&r) = tiles.get(1) {
        render_call_stack(dv, r, focus == DashPane::CallStack, d.stack_sel, buf);
    }
    if let Some(&r) = tiles.get(2) {
        render_goroutines(dv, r, focus == DashPane::Goroutines, d.gor_sel, buf);
    }
}

/// The selection to pass a tile widget: `Some(clamped)` on the focused tile with
/// rows, `None` otherwise (an unfocused / empty tile shows no cursor).
fn tile_sel(focused: bool, sel: usize, len: usize) -> Option<usize> {
    if focused && len > 0 {
        Some(sel.min(len - 1))
    } else {
        None
    }
}

/// Compute the scroll offset that keeps `sel` visible in a `rows`-tall list
/// within `inner`, and -- when the content overflows the tile -- draw a
/// right-edge Scrollbar, returning the content rect one column narrower to
/// clear the bar. When it fits (or there is no selection) the offset is 0, no
/// bar, and the full inner rect. Stateless: a scrolled selection bottom-anchors
/// (it sits on the window's last row), which needs no stored offset.
fn scrollable(inner: Rect, rows: usize, sel: Option<usize>, buf: &mut Buffer) -> (Rect, usize) {
    let h = inner.height as usize;
    if inner.is_empty() || h == 0 || rows <= h {
        return (inner, 0);
    }
    let off = match sel {
        Some(s) => s.min(rows - 1).saturating_sub(h - 1).min(rows - h),
        None => 0,
    };
    Scrollbar::new(rows, h, off)
        .style(theme::tile_scroll_track())
        .thumb_style(theme::tile_scroll_thumb())
        .render(inner, buf);
    let content = Rect::new(inner.x, inner.y, inner.width.saturating_sub(1), inner.height);
    (content, off)
}

/// Variables: a `locals` group node with the current frame's locals as leaves
/// (`name = value`). `l`/`h` collapse the group; the per-variable nested expand
/// is a later leg. `sel` indexes the flattened rows (the group node, then the
/// leaves), highlighted only when `focused`.
fn render_variables(
    dv: &DebugView,
    area: Rect,
    focused: bool,
    sel: usize,
    expanded: bool,
    buf: &mut Buffer,
) {
    let inner = tile(area, " Variables ", focused, buf);
    let labels: alloc::vec::Vec<String> = dv
        .locals
        .iter()
        .map(|v| format!("{} = {}", v.name, v.value))
        .collect();
    let children: alloc::vec::Vec<TreeItem> = labels
        .iter()
        .map(|s| TreeItem::leaf(s).style(theme::tile_text()))
        .collect();
    let roots = [TreeItem::node("locals", children)
        .style(theme::tile_dim())
        .expanded(expanded)];
    let rows = flatten_tree(&roots);
    let sel = tile_sel(focused, sel, rows.len());
    let (content, off) = scrollable(inner, rows.len(), sel, buf);
    Tree::new(&rows)
        .select(sel)
        .selected_style(theme::tile_selected())
        .offset(off)
        .render(content, buf);
}

/// Call Stack: `#idx func` and its `file:line`, top frame first. The
/// cross-boundary `── kernel ──` divider (section 5) fills in at 8f-3.
fn render_call_stack(dv: &DebugView, area: Rect, focused: bool, sel: usize, buf: &mut Buffer) {
    let inner = tile(area, " Call Stack ", focused, buf);
    let sel = tile_sel(focused, sel, dv.frames.len());
    let (content, off) = scrollable(inner, dv.frames.len(), sel, buf);
    // Two columns: "#0 func" (fills the tile less the location) and the location.
    let loc_w = 12u16.min(content.width / 2);
    let name_w = content.width.saturating_sub(loc_w).saturating_sub(1);
    let cols = [name_w, loc_w];
    let cells: alloc::vec::Vec<[String; 2]> = dv
        .frames
        .iter()
        .enumerate()
        .map(|(i, f)| [format!("#{} {}", i, f.func), f.location.clone()])
        .collect();
    let refs: alloc::vec::Vec<[&str; 2]> =
        cells.iter().map(|c| [c[0].as_str(), c[1].as_str()]).collect();
    let rows: alloc::vec::Vec<Row> = refs.iter().map(|c| Row::new(&c[..])).collect();
    Table::new(&cols, &rows)
        .style(theme::tile_text())
        .select(sel)
        .selected_style(theme::tile_selected())
        .offset(off)
        .render(content, buf);
}

/// Goroutines: `g<id>` and the debugger's one-line state.
fn render_goroutines(dv: &DebugView, area: Rect, focused: bool, sel: usize, buf: &mut Buffer) {
    let inner = tile(area, " Goroutines ", focused, buf);
    let sel = tile_sel(focused, sel, dv.goroutines.len());
    let (content, off) = scrollable(inner, dv.goroutines.len(), sel, buf);
    let id_w = 5u16.min(content.width);
    let state_w = content.width.saturating_sub(id_w).saturating_sub(1);
    let cols = [id_w, state_w];
    let cells: alloc::vec::Vec<[String; 2]> = dv
        .goroutines
        .iter()
        .map(|g| [format!("g{}", g.id), g.state.clone()])
        .collect();
    let refs: alloc::vec::Vec<[&str; 2]> =
        cells.iter().map(|c| [c[0].as_str(), c[1].as_str()]).collect();
    let rows: alloc::vec::Vec<Row> = refs.iter().map(|c| Row::new(&c[..])).collect();
    Table::new(&cols, &rows)
        .style(theme::tile_text())
        .select(sel)
        .selected_style(theme::tile_selected())
        .offset(off)
        .render(content, buf);
}

/// The bottom Console: a `[Program] Debug` tab strip, the current status line,
/// then the tail of the scrollback. The Program/Debug stream split (the real
/// pts vs the REPL) is 8f-2b/3; at 8f-2a both tabs show the unified log.
fn render_console(ed: &Editor, dv: &DebugView, area: Rect, buf: &mut Buffer) {
    let focused = ed.dash().focus == DashPane::Console;
    let inner = tile(area, " Console ", focused, buf);
    if inner.is_empty() {
        return;
    }
    // Row 0: the tab strip.
    Tabs::new(&["Program", "Debug"])
        .active(ed.dash().console_tab)
        .style(theme::tile_dim())
        .active_style(theme::tile_title(true))
        .render(inner, buf);
    // Row 1: the persistent status line (ember).
    if inner.height > 1 {
        let y = inner.y + 1;
        draw_text(buf, inner.x, y, inner.right(), &dv.status, theme::status_msg());
    }
    // Rows 2..: the tail of the scrollback.
    let body_h = inner.height.saturating_sub(2) as usize;
    if body_h > 0 && !dv.console.is_empty() {
        let first = dv.console.len().saturating_sub(body_h);
        let refs: alloc::vec::Vec<&str> =
            dv.console[first..].iter().map(|s| s.as_str()).collect();
        let body = Rect::new(inner.x, inner.y + 2, inner.width, body_h as u16);
        List::new(&refs).style(theme::tile_text()).render(body, buf);
    }
}

/// Render just the editor (gutter + text + overlays + status) into `area`. This
/// is the whole pre-8f-2 `render`; the dashboard calls it for the editor tile.
fn render_editor(ed: &Editor, area: Rect, buf: &mut Buffer) -> (u16, u16) {
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

    // The completion popup overlays Insert and owns the cursor (it is a list
    // the user is steering), so it is checked with the other selectable
    // overlays rather than drawn as a passive box.
    if let Some((items, sel)) = ed.completion_state() {
        return render_completion(items, sel, text_area, buf);
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
    // Hover paints LAST, over the text and even over the block cursor -- it is
    // a transient answer to a question the user just asked, so it should be
    // the most visible thing on screen for the one keystroke it survives. It
    // deliberately does NOT take the cursor: hover is non-modal, and moving
    // the cursor into a popup the next key dismisses would be disorienting.
    if let Some(text) = ed.hover() {
        render_hover(text, text_area, buf);
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
/// The gutter style for logical row `row`: a diagnostic RECOLORS the line
/// number (error rust / warning gold), and it wins over the current-line tint
/// -- the cursor's own position is already obvious from the lifted row, so the
/// scarce signal is the error. `None` from the store leaves the base style.
fn gutter_style_for(ed: &Editor, row: usize, base: Style) -> Style {
    match ed.diags.severity_of_line(row) {
        Some(DiagSeverity::Error) => theme::gutter_error(),
        Some(_) => theme::gutter_warn(),
        None => base,
    }
}

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
        buf.set_str(text_area.x, y, &num, gutter_style_for(ed, row, gut_style));
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
            buf.set_str(text_area.x, y, &num, gutter_style_for(ed, row, gut_style));
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

/// The completion popup (Ctrl-N in Insert): the server's candidates, selectable.
/// Owns the cursor like the other pickers.
fn render_completion(
    items: &[crate::editor::Candidate],
    sel: usize,
    text_area: Rect,
    buf: &mut Buffer,
) -> (u16, u16) {
    // Window FIRST, then format: the visible slice is chosen before any row is
    // built, so only what is drawn costs a string -- and, more importantly, the
    // selection is always inside the rendered list. Formatting the leading N
    // and windowing afterwards silently drops the highlight off the end once
    // the selection passes row N.
    let first = sel.saturating_sub(COMPLETION_ROWS.saturating_sub(1));
    let rows: alloc::vec::Vec<String> = items
        .iter()
        .skip(first)
        .take(COMPLETION_ROWS)
        .map(|c| match &c.detail {
            Some(d) => format!("{}  {}", c.label, d),
            None => c.label.clone(),
        })
        .collect();
    let refs: alloc::vec::Vec<&str> = rows.iter().map(|s| s.as_str()).collect();
    let title = " complete ";
    let inner = popup(text_area, title, widest(&refs, title), rows.len() as u16, buf);
    List::new(&refs)
        .select(Some(sel - first))
        .style(theme::palette_surface())
        .selected_style(theme::palette_selected())
        .render(inner, buf);
    let cy = inner
        .y
        .saturating_add((sel - first) as u16)
        .min(inner.bottom().saturating_sub(1));
    (inner.x, cy)
}

/// The hover popup: a language server's description of the symbol under the
/// cursor, wrapped to the text area and clipped to a few rows.
fn render_hover(text: &str, text_area: Rect, buf: &mut Buffer) {
    // gopls answers in markdown with fenced code blocks; the fences are noise
    // in a plain-text box, and a blank line between sections is worth keeping.
    let content_w = text_area.width.saturating_sub(4).max(1);
    let mut rows: alloc::vec::Vec<String> = alloc::vec::Vec::new();
    for line in text.lines() {
        if line.trim_start().starts_with("```") {
            continue;
        }
        if rows.len() >= HOVER_ROWS {
            break;
        }
        // Hard-wrap rather than truncate: a signature is the most useful part
        // of a hover and it is exactly what overflows one line.
        let mut rest = line;
        loop {
            let take = rest
                .char_indices()
                .nth(content_w as usize)
                .map(|(i, _)| i)
                .unwrap_or(rest.len());
            rows.push(String::from(&rest[..take]));
            rest = &rest[take..];
            if rest.is_empty() || rows.len() >= HOVER_ROWS {
                break;
            }
        }
    }
    if rows.is_empty() {
        return;
    }
    let refs: alloc::vec::Vec<&str> = rows.iter().map(|s| s.as_str()).collect();
    let title = " hover ";
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
    let (r, c) = ed.text.cursor();

    // The center slot, in priority order:
    //   1. an explicit status message -- the reply to something the user just
    //      did, so it must not be buried by an ambient diagnostic;
    //   2. the diagnostic on the CURSOR's line -- this is the whole "inline"
    //      surface: put the cursor on a marked line and read the message;
    //   3. the file name.
    let cur_diag = ed.diags.for_line(r);
    let (center, center_style) = match (&ed.status, cur_diag) {
        (Some(m), _) => (m.clone(), theme::status_msg()),
        (None, Some(d)) => {
            let style = match d.severity {
                DiagSeverity::Error => theme::status_error(),
                _ => theme::status_warn(),
            };
            (d.message.clone(), style)
        }
        (None, None) => {
            let mut s = String::from(name);
            if ed.modified {
                s.push_str(" [+]");
            }
            (s, theme::status_msg())
        }
    };

    // The right slot gains an error/warning tally when the buffer has any --
    // so a diagnostic off-screen is still visible as a count.
    let (errs, warns) = ed.diags.counts();
    let mut right = String::new();
    if errs > 0 || warns > 0 {
        right.push_str(&format!("{}E {}W  ", errs, warns));
    }
    match ed.buffer_indicator() {
        Some(b) => right.push_str(&format!("{} {}:{} ", b, r + 1, c + 1)),
        None => right.push_str(&format!("{}:{} ", r + 1, c + 1)),
    }

    StatusLine::new()
        .fill(theme::statusbar())
        .left(Span::styled(&mode, theme::mode_chip(mode_color(&ed.mode))))
        .center(Span::styled(&center, center_style))
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
        // Completion overlays Insert; keeping the accent means the cursor does
        // not change colour under the user just because a popup opened.
        Mode::Insert | Mode::Completion { .. } => theme::GREEN,
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

    fn diag(line: usize, sev: DiagSeverity, msg: &str) -> crate::diag::LineDiag {
        crate::diag::LineDiag {
            line,
            col: 0,
            end_col: 1,
            severity: sev,
            message: alloc::string::ToString::to_string(msg),
        }
    }

    /// The row text of `y`, trimmed -- for asserting on the status line.
    fn row_text(b: &Buffer, y: u16, w: u16) -> String {
        let mut s = String::new();
        for x in 0..w {
            s.push(b.get(x, y).map(|c| c.symbol).unwrap_or(' '));
        }
        s
    }

    #[test]
    fn a_diagnostic_recolors_its_gutter_number_only() {
        let mut ed = Editor::new(Some("f.go".into()), "aaa
bbb
ccc", false);
        ed.diags.set(alloc::vec![diag(1, DiagSeverity::Error, "undefined: zzz")]);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        // Row 1 (line 2) is the marked one; its number is rust + bold.
        let marked = b.get(2, 1).unwrap();
        assert_eq!(marked.symbol, '2');
        assert_eq!(marked.style.fg, theme::RUST);
        // Its NEIGHBOURS keep the ordinary gutter colour -- the tint must not
        // bleed to the whole buffer.
        assert_ne!(b.get(2, 2).unwrap().style.fg, theme::RUST);
        // And the line's TEXT is untouched (we recolor the gutter, not the code).
        assert_eq!(sym(&b, 4, 1), 'b');
        assert_ne!(b.get(4, 1).unwrap().style.fg, theme::RUST);
    }

    #[test]
    fn a_warning_uses_the_warn_colour_not_the_error_colour() {
        let mut ed = Editor::new(Some("f.go".into()), "aaa
bbb", false);
        ed.diags.set(alloc::vec![diag(0, DiagSeverity::Warning, "unused")]);
        let mut b = Buffer::empty(area());
        render(&ed, area(), &mut b);
        assert_eq!(b.get(2, 0).unwrap().style.fg, theme::GOLD);
    }

    #[test]
    fn the_cursor_lines_diagnostic_shows_on_the_status_line() {
        let mut ed = Editor::new(Some("f.go".into()), "aaa
bbb
ccc", false);
        ed.diags.set(alloc::vec![diag(0, DiagSeverity::Error, "undefined: zzz")]);
        let a = Rect::new(0, 0, 40, 5);
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        // Cursor starts on line 0 -> its message occupies the status centre.
        let status = row_text(&b, a.height - 1, a.width);
        assert!(status.contains("undefined: zzz"), "status was {:?}", status);
        // ...and the tally rides the right slot.
        assert!(status.contains("1E 0W"), "status was {:?}", status);
    }

    #[test]
    fn an_explicit_status_message_outranks_the_diagnostic() {
        let mut ed = Editor::new(Some("f.go".into()), "aaa", false);
        ed.diags.set(alloc::vec![diag(0, DiagSeverity::Error, "undefined: zzz")]);
        ed.set_status(alloc::string::ToString::to_string("saved 3 bytes"));
        let a = Rect::new(0, 0, 40, 5);
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        let status = row_text(&b, a.height - 1, a.width);
        // The reply to what the user just did must not be buried.
        assert!(status.contains("saved 3 bytes"), "status was {:?}", status);
        assert!(!status.contains("undefined"), "status was {:?}", status);
    }

    #[test]
    fn no_diagnostics_leaves_the_status_line_as_before() {
        let ed = Editor::new(Some("f.go".into()), "aaa", false);
        let a = Rect::new(0, 0, 40, 5);
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        let status = row_text(&b, a.height - 1, a.width);
        assert!(status.contains("f.go"), "status was {:?}", status);
        // No tally when the buffer is clean.
        assert!(!status.contains("0E"), "status was {:?}", status);
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
        ed.handle_key(KeyEvent::char('b')); // ":b" -> bn bp bd bd! break bt
        let mut b = Buffer::empty(big);
        let cur = render(&ed, big, &mut b);
        // 6 matches (the debugger `break` + `bt` join the buffer verbs) ->
        // box_h 8; by = 13-8 = 5; inner.y = 6; row 0 = ":bn  ..." (COMMANDS order).
        assert_eq!(sym(&b, 1, 6), ':');
        assert_eq!(sym(&b, 2, 6), 'b');
        assert_eq!(sym(&b, 3, 6), 'n');
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

    // -- the language-server overlays (8e-2c) ------------------------------

    /// Does any row of the rendered frame contain `needle`?
    fn frame_has(b: &Buffer, a: Rect, needle: &str) -> bool {
        (0..a.height).any(|y| row_text(b, y, a.width).contains(needle))
    }

    #[test]
    fn hover_draws_a_popup_over_the_text() {
        let a = Rect::new(0, 0, 30, 8);
        let mut ed = Editor::new(Some("f.go".into()), "package main", false);
        let mut b = Buffer::empty(a);
        ed.show_hover("func F(x int) error".to_string());
        render(&ed, a, &mut b);
        assert!(frame_has(&b, a, "hover"));
        assert!(frame_has(&b, a, "func F(x int) error"));
    }

    #[test]
    fn hover_strips_markdown_fences_and_is_row_bounded() {
        let a = Rect::new(0, 0, 30, 24);
        let mut ed = Editor::new(Some("f.go".into()), "x", false);
        let mut b = Buffer::empty(a);
        // gopls wraps signatures in fenced code blocks; a bare ``` row would
        // just be noise in a plain-text box.
        let mut long = String::from("```go\nfunc F()\n```\n");
        for i in 0..40 {
            long.push_str(&format!("line {}\n", i));
        }
        ed.show_hover(long);
        render(&ed, a, &mut b);
        assert!(frame_has(&b, a, "func F()"));
        assert!(!frame_has(&b, a, "```"));
        // Bounded: a documented symbol must not bury the code it describes.
        // HOVER_ROWS content + 2 border rows is the ceiling.
        assert!(!frame_has(&b, a, &format!("line {}", HOVER_ROWS + 2)));
    }

    #[test]
    fn a_long_hover_line_wraps_instead_of_truncating() {
        let a = Rect::new(0, 0, 20, 10);
        let mut ed = Editor::new(Some("f.go".into()), "x", false);
        let mut b = Buffer::empty(a);
        // A signature is the most useful part of a hover and exactly the part
        // that overflows one line.
        ed.show_hover("func Fetch(ctx context.Context) error".to_string());
        render(&ed, a, &mut b);
        assert!(frame_has(&b, a, "func Fetch"));
        assert!(frame_has(&b, a, "error")); // the tail survived the wrap
    }

    #[test]
    fn the_completion_popup_lists_candidates_and_marks_the_selection() {
        let a = Rect::new(0, 0, 30, 10);
        let mut ed = Editor::new(Some("f.go".into()), "", false);
        let mut b = Buffer::empty(a);
        ed.handle_key(KeyEvent::char('i'));
        ed.handle_key(KeyEvent::char('P'));
        ed.show_completion(alloc::vec![
            crate::editor::Candidate {
                label: "Print".into(),
                detail: Some("func(...any)".into()),
                insert: "Print".into(),
            },
            crate::editor::Candidate {
                label: "Println".into(),
                detail: None,
                insert: "Println".into(),
            },
        ]);
        let cur = render(&ed, a, &mut b);
        assert!(frame_has(&b, a, "complete"));
        assert!(frame_has(&b, a, "Print"));
        assert!(frame_has(&b, a, "func(...any)")); // detail shown after label
        assert!(frame_has(&b, a, "Println"));
        // The popup owns the cursor, parked on the selected row.
        assert_eq!(b.get(cur.0, cur.1).is_some(), true);
    }

    #[test]
    fn the_completion_popup_scrolls_the_selection_into_view() {
        let a = Rect::new(0, 0, 30, 20);
        let mut ed = Editor::new(Some("f.go".into()), "", false);
        let mut b = Buffer::empty(a);
        ed.handle_key(KeyEvent::char('i'));
        // More candidates than the popup can show at once -- a server can
        // easily offer hundreds.
        let items: alloc::vec::Vec<crate::editor::Candidate> = (0..COMPLETION_ROWS + 5)
            .map(|i| crate::editor::Candidate {
                label: format!("cand{}", i),
                detail: None,
                insert: format!("cand{}", i),
            })
            .collect();
        let last = items.len() - 1;
        ed.show_completion(items);
        // Walk to the last candidate.
        for _ in 0..last {
            ed.handle_key(KeyEvent::with(KeyCode::Char('n'), kaua::event::Mods::CTRL));
        }
        render(&ed, a, &mut b);
        // A popup that cannot show the highlighted candidate is useless.
        assert!(frame_has(&b, a, &format!("cand{}", last)));
        assert!(!frame_has(&b, a, "cand0 "));
    }

    // -- the debugger dashboard (8f-2) -------------------------------------

    fn dbg_ed() -> Editor {
        let mut ed = Editor::new(Some("m.go".into()), "hello\nworld", false);
        ed.set_debug_view(Some(crate::debug::DebugView {
            status: "stopped: breakpoint at main.parkLoop".into(),
            frames: alloc::vec![crate::debug::StackRow {
                func: "main.parkLoop".into(),
                location: "child.go:23".into(),
            }],
            locals: alloc::vec![crate::debug::VarRow {
                name: "i".into(),
                value: "3".into(),
            }],
            goroutines: alloc::vec![crate::debug::GoroutineRow {
                id: 1,
                state: "running".into(),
            }],
            console: alloc::vec!["stopped".into()],
        }));
        ed
    }

    #[test]
    fn the_dashboard_tiles_and_editor_coexist_when_roomy() {
        let a = Rect::new(0, 0, 60, 20);
        let ed = dbg_ed();
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        // The three sidebar tiles + the bottom Console are all titled.
        assert!(frame_has(&b, a, "Variables"));
        assert!(frame_has(&b, a, "Call Stack"));
        assert!(frame_has(&b, a, "Goroutines"));
        assert!(frame_has(&b, a, "Console"));
        // The tiles carry live DAP data (the Call Stack shows the frame).
        assert!(frame_has(&b, a, "parkLoop"));
        // The editor still renders in its sub-rect (gutter "1" at col 2).
        assert_eq!(sym(&b, 2, 0), '1');
        // The scroll width is the narrowed sub-rect, not the full area, so the
        // binary scrolls to the same width render draws into.
        assert_eq!(editor_area(&ed, a).width, 60 - SIDEBAR_W);
    }

    #[test]
    fn the_dashboard_collapses_on_a_small_terminal() {
        // Below the floor the editor stays full-width -- the debug data still
        // reaches the status line + `:bt`.
        let a = Rect::new(0, 0, 40, 20); // width < DASH_MIN_W
        let ed = dbg_ed();
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        assert!(!frame_has(&b, a, "Variables"));
        assert_eq!(editor_area(&ed, a).width, 40);
    }

    #[test]
    fn no_dashboard_without_a_session() {
        // A normal editor (no pushed view) is full-width -- the pre-8f render,
        // byte-for-byte (this is why the 30+ tests above never changed).
        let a = Rect::new(0, 0, 60, 20);
        let ed = Editor::new(Some("m.go".into()), "hello", false);
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        assert!(!frame_has(&b, a, "Variables"));
        assert_eq!(editor_area(&ed, a).width, 60);
    }

    #[test]
    fn the_focused_tile_takes_an_ember_border() {
        let a = Rect::new(0, 0, 60, 20);
        let mut ed = dbg_ed();
        // Default focus is the editor -> the Variables tile border is dim. Its
        // top-left corner sits at the sidebar origin (60 - 28, 0).
        let corner_x = 60 - SIDEBAR_W;
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        assert_eq!(b.get(corner_x, 0).unwrap().style.fg, theme::BORDER);
        // Tab focuses Variables -> its border goes ember.
        ed.handle_key(KeyEvent::new(KeyCode::Tab));
        let mut b2 = Buffer::empty(a);
        render(&ed, a, &mut b2);
        assert_eq!(b2.get(corner_x, 0).unwrap().style.fg, theme::EMBER);
    }

    // -- the navigable dashboard (8f-2b-1) ---------------------------------

    fn dbg_ed_n(frames: usize, locals: usize, gors: usize) -> Editor {
        let mut ed = Editor::new(Some("m.go".into()), "hello\nworld", false);
        ed.set_debug_view(Some(crate::debug::DebugView {
            status: "stopped".into(),
            frames: (0..frames)
                .map(|i| crate::debug::StackRow {
                    func: format!("f{}", i),
                    location: format!("m.go:{}", i),
                })
                .collect(),
            locals: (0..locals)
                .map(|i| crate::debug::VarRow {
                    name: format!("v{}", i),
                    value: i.to_string(),
                })
                .collect(),
            goroutines: (0..gors)
                .map(|i| crate::debug::GoroutineRow {
                    id: i as i64,
                    state: "running".into(),
                })
                .collect(),
            console: alloc::vec!["boot".into()],
        }));
        ed
    }

    /// The first buffer row whose text contains `needle`.
    fn find_row(b: &Buffer, a: Rect, needle: &str) -> Option<u16> {
        (0..a.height).find(|&y| row_text(b, y, a.width).contains(needle))
    }

    /// Does any cell on row `y` carry background `bg`? (A selected tile row is a
    /// full-width highlight; the ember tile border/title use ember *fg* over the
    /// slate bg, so only a selection paints an ember *bg*.)
    fn row_has_bg(b: &Buffer, a: Rect, y: u16, bg: kaua::style::Color) -> bool {
        (0..a.width).any(|x| b.get(x, y).map(|c| c.style.bg == bg).unwrap_or(false))
    }

    #[test]
    fn the_focused_tiles_selected_row_is_highlighted() {
        let a = Rect::new(0, 0, 60, 24);
        let mut ed = dbg_ed_n(3, 0, 0);
        // Focus the Call Stack (Tab x2) and select frame #1 (j).
        ed.handle_key(KeyEvent::new(KeyCode::Tab));
        ed.handle_key(KeyEvent::new(KeyCode::Tab));
        ed.handle_key(KeyEvent::char('j'));
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        let y = find_row(&b, a, "f1").expect("frame #1 shown");
        assert!(row_has_bg(&b, a, y, theme::EMBER), "selected row is ember");
        // The other frames are not highlighted.
        let y0 = find_row(&b, a, "f0").expect("frame #0 shown");
        assert!(!row_has_bg(&b, a, y0, theme::EMBER));
    }

    #[test]
    fn an_unfocused_tile_shows_no_row_cursor() {
        // Focus stays on the editor -> the tiles render their data but no tile
        // row carries a selection highlight (§2.3: the cursor is on the focused
        // tile only).
        let a = Rect::new(0, 0, 60, 24);
        let ed = dbg_ed_n(3, 0, 0);
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        let y = find_row(&b, a, "f0").expect("a frame is shown");
        assert!(!row_has_bg(&b, a, y, theme::EMBER));
    }

    #[test]
    fn a_long_tile_scrolls_to_the_selection_and_shows_a_scrollbar() {
        // More frames than the Call Stack tile is tall: selecting the last one
        // scrolls it into view and a scrollbar thumb (█) appears.
        let a = Rect::new(0, 0, 60, 30);
        let mut ed = dbg_ed_n(12, 0, 0);
        ed.handle_key(KeyEvent::new(KeyCode::Tab));
        ed.handle_key(KeyEvent::new(KeyCode::Tab)); // focus Call Stack
        ed.handle_key(KeyEvent::char('G')); // select the last frame
        let mut b = Buffer::empty(a);
        render(&ed, a, &mut b);
        assert!(find_row(&b, a, "f11").is_some(), "the selected frame is visible");
        assert!(find_row(&b, a, "f0").is_none(), "the top frames scrolled off");
        // A scrollbar thumb glyph is drawn nowhere else in the frame.
        let has_thumb = (0..a.width)
            .any(|x| (0..a.height).any(|y| b.get(x, y).map(|c| c.symbol) == Some('\u{2588}')));
        assert!(has_thumb, "an overflowing tile shows a scrollbar");
    }

    #[test]
    fn collapsing_the_locals_group_hides_the_leaves() {
        let a = Rect::new(0, 0, 60, 24);
        let mut ed = dbg_ed_n(0, 2, 0);
        ed.handle_key(KeyEvent::new(KeyCode::Tab)); // focus Variables
        let mut open = Buffer::empty(a);
        render(&ed, a, &mut open);
        assert!(frame_has(&open, a, "v0"), "leaves show while expanded");
        ed.handle_key(KeyEvent::char('h')); // collapse the group
        let mut shut = Buffer::empty(a);
        render(&ed, a, &mut shut);
        assert!(frame_has(&shut, a, "locals"), "the group node stays");
        assert!(!frame_has(&shut, a, "v0"), "the leaves are hidden");
    }
}
