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

/// Draw one frame: header (2 rows) + process table (fills) + [detail pane] +
/// footer (1 row). The detail pane appears only when toggled (`d`).
pub fn render(term: &mut Terminal, app: &App) -> Result<()> {
    let area = term.area();
    let cur = app.cur_index();
    {
        let buf = term.back_mut();
        buf.reset();
        if app.show_detail {
            // Reserve a bottom pane for the per-thread scheduler detail. Cap it so
            // the process list always keeps at least a few rows.
            let detail_h = detail_height(app, area.height);
            let chunks = Layout::vertical(&[
                Constraint::Length(2),
                Constraint::Min(1),
                Constraint::Length(detail_h),
                Constraint::Length(1),
            ])
            .split(area);
            render_header(buf, chunks[0], app);
            render_table(buf, chunks[1], app, cur);
            render_detail(buf, chunks[2], app);
            render_footer(buf, chunks[3], app);
        } else {
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
    }
    term.set_cursor(None); // a monitor has no text cursor
    term.flush()
}

/// The detail pane height: 1 title + 1 column header + one row per thread, capped
/// so the process list keeps >= 3 rows (and never negative on a tiny console).
fn detail_height(app: &App, total_h: u16) -> u16 {
    let want = match &app.detail {
        Some(d) => 2 + d.threads.len() as u16,
        None => 2, // the "unavailable" line + a title
    };
    // Leave header(2) + a >=3-row list + footer(1) = 6 for the rest.
    let cap = total_h.saturating_sub(6);
    want.min(cap.max(1))
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
        // prowl-3c: one mini-bar per core (the differentiator). Falls back to the
        // aggregate meter when /ctl/cpu was empty (e.g. an early first frame).
        if app.cpus.is_empty() {
            render_meter(buf, area, y + 1, app);
        } else {
            render_cpubars(buf, area, y + 1, app);
        }
    }
}

/// `0[███░] 1[█░░░] 2[░░░░] 3[██░░]` -- one labeled mini-bar per online CPU, its
/// fill = that core's utilization (idle_ns diffed across polls). Segment width is
/// computed to fit the row; a too-narrow console degrades to as many cores as fit.
fn render_cpubars(buf: &mut Buffer, area: Rect, y: u16, app: &App) {
    let n = app.cpus.len().max(1) as u16;
    let avail = area.width;
    // Each segment: "<label>[<bar>] ". Label is 1-2 digits + "[" + "]" + " " ~= 4
    // fixed chars; give the bar the rest, clamped to [1, 6].
    let seg_w = (avail / n).max(1);
    let bar_w = seg_w.saturating_sub(4).clamp(1, 6);
    let mut x = area.x;
    for c in &app.cpus {
        if x >= area.right() {
            break;
        }
        x = buf.set_str(x, y, &format!("{}", c.cpu), dim());
        x = buf.set_str(x, y, "[", dim());
        let filled = (c.util_x10 * bar_w as u64 / 1000) as u16;
        for i in 0..bar_w {
            if x >= area.right() {
                break;
            }
            let (ch, st) = if i < filled {
                ('\u{2588}', meter_fill()) // █
            } else {
                ('\u{2591}', meter_empty()) // ░
            };
            buf.set_cell(x, y, Cell::new(ch, st));
            x = x.saturating_add(1);
        }
        x = buf.set_str(x, y, "] ", dim());
    }
}

/// The per-thread scheduler detail for the selected process (`/proc/<pid>/sched`),
/// or an "unavailable" line when the OQ-4 gate denied the read (not owner / no
/// CAP_HOSTOWNER) or the process exited.
fn render_detail(buf: &mut Buffer, area: Rect, app: &App) {
    if area.is_empty() {
        return;
    }
    let y = area.y;
    match &app.detail {
        None => {
            buf.set_str(
                area.x,
                y,
                "sched detail unavailable (not owner / no CAP_HOSTOWNER, or exited)",
                dim(),
            );
        }
        Some(d) => {
            buf.set_str(
                area.x,
                y,
                &format!("sched: {} (pid {})  tid band cpu run_ns nsched parks nmig state", d.name, d.pid),
                head(),
            );
            let mut row = y + 1;
            for t in &d.threads {
                if row >= area.bottom() {
                    break;
                }
                buf.set_str(
                    area.x,
                    row,
                    &format!(
                        "  {:<4} {:<4} {:<3} {:<12} {:<8} {:<8} {:<5} {}",
                        t.tid, t.band, t.cpu, t.run_ns, t.nsched, t.parks, t.nmig, t.state
                    ),
                    normal(),
                );
                row += 1;
            }
        }
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

    // prowl-4: the display order + per-row depth. Tree mode reorders
    // parent-before-child (sample::tree_order over the ppid edges) and indents the
    // NAME by depth; flat mode is the identity order at depth 0. Each entry is
    // (index-into-app.rows, depth).
    let order: Vec<(usize, usize)> = if app.show_tree {
        crate::sample::tree_order(&app.rows)
    } else {
        (0..n).map(|i| (i, 0usize)).collect()
    };
    // The selected row's POSITION in the display order (the cursor tracks a pid,
    // and tree order is a permutation of app.rows) -- for the scroll + highlight.
    let sel_disp = order.iter().position(|&(oi, _)| oi == cur).unwrap_or(0);
    let offset = compute_offset(sel_disp, order.len(), view_rows);

    // The Table borrows &[&str] cells, so the owned display strings must outlive
    // the Row borrows -- keep `cells` + `cell_refs` alive through the render call.
    let mut cells: Vec<[String; 6]> = Vec::with_capacity(order.len());
    for &(oi, depth) in &order {
        let r = &app.rows[oi];
        let name = if app.show_tree && depth > 0 {
            // Indent by depth + a light branch connector; the NAME column width
            // (COLUMNS[1]) bounds a deep tree via the Table's own truncation.
            let mut s = String::new();
            for _ in 0..depth {
                s.push_str("  ");
            }
            s.push_str("\u{2514} "); // └
            s.push_str(&r.name);
            s
        } else {
            r.name.clone()
        };
        cells.push([
            r.pid.to_string(),
            name,
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
        .select(Some(sel_disp))
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
        " up/dn move  d detail  t tree  z stop  c cont  k kill  s sort  q quit ",
        dim(),
    );
    // prowl-4: reflect the tree toggle beside the sort/status on the right. The
    // token is UPPERCASE (TREE/FLAT) so it is distinct from the lowercase "t tree"
    // key hint on the left -- the E2E can assert the toggle unambiguously.
    let mode = if app.show_tree { "TREE" } else { "FLAT" };
    let right = match &app.status {
        Some(s) => format!("[{}]  {} sort:{} ", s, mode, app.sort.label()),
        None => format!("{} sort:{} ", mode, app.sort.label()),
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
