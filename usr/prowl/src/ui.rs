// prowl::ui -- the Kaua rendering (a CPU meter header + the process table + a
// footer). Pure draw over the back buffer; the caller owns the App state.

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use kaua::buffer::{Buffer, Cell};
use kaua::layout::{Constraint, Layout};
use kaua::rect::Rect;
use kaua::style::{Attr, Color, Style};
use kaua::term::Terminal;
use kaua::widget::{Row, Table, Widget};

use libthyla_rs::err::Result;

use crate::sample::Sampler;
use crate::App;

// A restrained, Bonfire-adjacent palette (docs/UTOPIA-VISUAL.md); tasteful
// defaults, not pinned scripture.
fn ember() -> Style {
    Style::new().fg(Color::Rgb(0xE0, 0x78, 0x40)).attr(Attr::BOLD)
}
fn dim() -> Style {
    Style::new().fg(Color::Rgb(0x9a, 0x8f, 0x86))
}
fn head() -> Style {
    Style::new().fg(Color::Rgb(0xc9, 0xc1, 0xb8)).attr(Attr::BOLD)
}
fn normal() -> Style {
    Style::new().fg(Color::Rgb(0xd8, 0xcf, 0xc6))
}
fn selected() -> Style {
    Style::new().attr(Attr::REVERSE)
}
fn meter_fill() -> Style {
    Style::new().fg(Color::Rgb(0xE0, 0x78, 0x40))
}
fn meter_empty() -> Style {
    Style::new().fg(Color::Rgb(0x55, 0x50, 0x4c))
}

const COLUMNS: [u16; 6] = [6, 18, 8, 9, 5, 8];
const HEADERS: [&str; 6] = ["PID", "NAME", "%CPU", "MEM(pg)", "THR", "STATE"];

/// Draw one frame: header (2 rows) + process table (fills) + footer (1 row).
pub fn render(term: &mut Terminal, app: &App) -> Result<()> {
    let area = term.area();
    let cur = app.cur_index();
    {
        let buf = term.back_mut();
        buf.reset();
        let chunks = Layout::vertical(&[
            Constraint::Length(2),
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(area);
        render_header(buf, chunks[0], app);
        render_table(buf, chunks[1], app, cur);
        render_footer(buf, chunks[2], app);
    }
    term.set_cursor(None); // a monitor has no text cursor
    term.flush()
}

fn render_header(buf: &mut Buffer, area: Rect, app: &App) {
    if area.height == 0 {
        return;
    }
    let y = area.y;
    let mut x = buf.set_str(area.x, y, "prowl", ember());
    x = buf.set_str(x, y, &format!("   procs: {}", app.rows.len()), dim());
    buf.set_str(x, y, &format!("   cpus: {}", app.ncpus), dim());
    if area.height >= 2 {
        render_meter(buf, area, y + 1, app);
    }
}

/// `CPU [████░░░░] 47.3%  total 189.2%` -- the aggregate system meter. Util is
/// the sum of every proc's %CPU divided by the core count (0..=100%); total is
/// the raw sum (can exceed 100% across cores).
fn render_meter(buf: &mut Buffer, area: Rect, y: u16, app: &App) {
    let total_x10 = Sampler::total_pct_x10(&app.rows);
    let ncpus = app.ncpus.max(1) as u64;
    let util_x10 = (total_x10 / ncpus).min(1000);

    let trailing = format!(" {}%  total {}%", fmt_pct1(util_x10), fmt_pct1(total_x10));
    let trailing_w = trailing.chars().count() as u16;

    let x0 = buf.set_str(area.x, y, "CPU ", dim());
    // Reserve "[" + bar + "]" + trailing within the row.
    let avail = area.right().saturating_sub(x0);
    let bar_w = avail.saturating_sub(2 + trailing_w);

    let mut x = buf.set_str(x0, y, "[", dim());
    let filled = if bar_w == 0 {
        0
    } else {
        (util_x10 * bar_w as u64 / 1000) as u16
    };
    for i in 0..bar_w {
        let (ch, st) = if i < filled {
            ('\u{2588}', meter_fill()) // █
        } else {
            ('\u{2591}', meter_empty()) // ░
        };
        buf.set_cell(x.saturating_add(i), y, Cell::new(ch, st));
    }
    x = x.saturating_add(bar_w);
    x = buf.set_str(x, y, "]", dim());
    buf.set_str(x, y, &trailing, normal());
}

fn render_table(buf: &mut Buffer, area: Rect, app: &App, cur: usize) {
    if area.is_empty() {
        return;
    }
    let n = app.rows.len();
    let view_rows = area.height.saturating_sub(1) as usize; // minus the header row
    let offset = compute_offset(cur, n, view_rows);

    // The Table borrows &[&str] cells, so the owned display strings must outlive
    // the Row borrows -- keep `cells` + `cell_refs` alive through the render call.
    let mut cells: Vec<[String; 6]> = Vec::with_capacity(n);
    for r in &app.rows {
        cells.push([
            r.pid.to_string(),
            r.name.clone(),
            fmt_pct1(r.cpu_pct_x10),
            r.pages.to_string(),
            r.threads.to_string(),
            r.state.as_str().to_string(),
        ]);
    }
    let cell_refs: Vec<[&str; 6]> = cells
        .iter()
        .map(|c| {
            [
                c[0].as_str(),
                c[1].as_str(),
                c[2].as_str(),
                c[3].as_str(),
                c[4].as_str(),
                c[5].as_str(),
            ]
        })
        .collect();
    let rows: Vec<Row> = cell_refs.iter().map(|c| Row::new(c)).collect();

    Table::new(&COLUMNS, &rows)
        .header(&HEADERS)
        .header_style(head())
        .style(normal())
        .select(Some(cur))
        .selected_style(selected())
        .offset(offset)
        .render(area, buf);
}

fn render_footer(buf: &mut Buffer, area: Rect, app: &App) {
    if area.is_empty() {
        return;
    }
    let y = area.y;

    // A pending kill takes over the footer as an unmissable confirm prompt.
    if let Some((pid, name)) = &app.confirm_kill {
        let st = Style::new()
            .fg(Color::Rgb(0x20, 0x1a, 0x16))
            .bg(Color::Rgb(0xE0, 0x78, 0x40))
            .attr(Attr::BOLD);
        for x in area.x..area.right() {
            buf.set_cell(x, y, Cell::new(' ', st));
        }
        buf.set_str(area.x, y, &format!(" kill {} \"{}\"?   y = yes    n = cancel ", pid, name), st);
        return;
    }

    let x = buf.set_str(
        area.x,
        y,
        " up/down move   k kill   s sort   r refresh   q quit ",
        dim(),
    );
    let right = match &app.status {
        Some(s) => format!("[{}]  sort:{} ", s, app.sort.label()),
        None => format!("sort:{} ", app.sort.label()),
    };
    let rx = area.right().saturating_sub(right.chars().count() as u16);
    if rx > x {
        buf.set_str(rx, y, &right, dim());
    }
}

/// The scroll position that keeps the selected row visible: bottom-anchored once
/// the cursor passes the last visible row, clamped so the final page is full.
fn compute_offset(cur: usize, n: usize, view: usize) -> usize {
    if view == 0 || n <= view {
        return 0;
    }
    let max_off = n - view;
    if cur < view {
        0
    } else {
        (cur + 1 - view).min(max_off)
    }
}

/// Tenths-of-a-percent -> "NN.N" (473 -> "47.3", 1000 -> "100.0").
fn fmt_pct1(x10: u64) -> String {
    format!("{}.{}", x10 / 10, x10 % 10)
}
