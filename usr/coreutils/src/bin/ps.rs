// ps [--color[=WHEN]] [--beacon=WHEN] -- list processes, the Thylacine way.
//
// Every Proc /proc lists, one row each from its /proc/<pid>/status (Plan 9's
// ps): the directory is whole at any size, where the /ctl/procs table holds
// only the rows that fit it. Each row is one instant of its Proc; the set is
// not one instant (a Proc that ends during the listing is skipped). Columns PID
// PPID NAME STATE THREADS PAGES TABLES CHILDREN CPU_NS, ascending by pid.
// Styled (a presentation tool, color auto): a boxed listing with CPU humanized
// (ns -> ms/s) and the state colored (ALIVE green, ZOMBIE ember, STOPPED gold).
// Color off: the plain table in /ctl/procs's own shape (the header, four-space
// columns, CPU_NS raw) -- parseable and byte-clean. A status that cannot be
// read or parsed fails the listing; ps never prints a table it knows is short.
//
// Beacon (docs/BEACON.md): at the Rich tier the listing is a beacon `table`
// (plain-aligned payload + frames, no box) with `obj type=pid` on the PID
// cells -- the presentation Halcyon's verb menu acts on.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::fmt::Write as _;
use coreutils::color::{self, ColorMode};
use coreutils::{boxd, palette, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::procs;
use libthyla_rs::{eprintln, io};

const USAGE: &str = "\
usage: ps [--color[=WHEN]] [--beacon=WHEN]
  List every process (/proc, one row per status): PID, parent, name, state,
  threads, pages, page tables, children, CPU time. Color off prints the plain
  table (parseable; CPU time in ns).
  --color[=WHEN]  colorize: always | never | auto (default)
  --beacon=WHEN   semantic markup: auto (default) | always | never
  --help  show this help

Examples:
  ps                    # boxed, colored on the console; plain in a pipe
  ps --color=never      # the plain table, byte-clean
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

struct PsRow {
    pid: String,
    ppid: String,
    name: String,
    state: String,
    threads: String,
    pages: String,
    tables: String,
    children: String,
    cpu: String, // humanized
}

/// ns -> a compact human figure: "<n>ms" under 10s, else "<n>s". The kernel
/// withholds another principal's CPU time, which shows as "-".
fn cpu_str(ns: Option<u64>) -> String {
    let Some(ns) = ns else {
        return String::from("-");
    };
    let ms = ns / 1_000_000;
    if ms < 10_000 {
        format!("{}ms", ms)
    } else {
        format!("{}s", ms / 1000)
    }
}

fn styled_row(r: &procs::Row) -> PsRow {
    PsRow {
        pid: format!("{}", r.pid),
        ppid: format!("{}", r.ppid),
        name: r.name.clone(),
        state: String::from(r.state.as_str()),
        threads: format!("{}", r.threads),
        pages: format!("{}", r.pages),
        tables: format!("{}", r.tables),
        children: format!("{}", r.children),
        cpu: cpu_str(r.cpu_ns),
    }
}

/// The plain table: /ctl/procs's header and row shape, so what parsed that file
/// parses this.
fn render_plain(out: &mut io::OutSink, rows: &[procs::Row]) {
    out.put(b"PID    PPID    NAME    STATE    THREADS    PAGES    TABLES    CHILDREN    CPU_NS\n");
    for r in rows {
        let _ = write!(
            out,
            "{}    {}    {}    {}    {}    {}    {}    {}    ",
            r.pid,
            r.ppid,
            r.name,
            r.state.as_str(),
            r.threads,
            r.pages,
            r.tables,
            r.children
        );
        match r.cpu_ns {
            Some(ns) => {
                let _ = write!(out, "{}\n", ns);
            }
            None => out.put(b"-\n"),
        }
    }
}

/// The kernel's STATE vocabulary (proc_state_word): ALIVE / ZOMBIE / STOPPED
/// (job-stop) / INVALID / "?".
fn state_color(state: &str) -> &'static str {
    match state {
        "ALIVE" => palette::GREEN,
        "ZOMBIE" => palette::EMBER,
        "STOPPED" => palette::GOLD,
        _ => palette::FG,
    }
}

const HDR: [&str; 9] = ["PID", "PPID", "NAME", "STATE", "THR", "PAGES", "TBL", "KIDS", "CPU"];

/// Column cells for one row, header-order.
fn cells(r: &PsRow) -> [&str; 9] {
    [&r.pid, &r.ppid, &r.name, &r.state, &r.threads, &r.pages, &r.tables, &r.children, &r.cpu]
}

/// r/l alignment per column: numerics right, NAME/STATE left.
const ALIGN: [u8; 9] = [b'r', b'r', b'l', b'l', b'r', b'r', b'r', b'r', b'r'];

fn pad_cell(out: &mut String, text: &str, width: usize, align: u8) {
    let pad = width.saturating_sub(text.chars().count());
    if align == b'r' {
        for _ in 0..pad {
            out.push(' ');
        }
        out.push_str(text);
    } else {
        out.push_str(text);
        for _ in 0..pad {
            out.push(' ');
        }
    }
}

/// The boxed cells realization (color on): the ls -l furniture, the state +
/// name colored, everything else plain.
fn render_box(out: &mut io::OutSink, rows: &[PsRow], on: bool) {
    let mut w = [0usize; 9];
    for (i, h) in HDR.iter().enumerate() {
        w[i] = h.chars().count();
    }
    for r in rows {
        for (i, c) in cells(r).iter().enumerate() {
            w[i] = w[i].max(c.chars().count());
        }
    }
    // Total visible content width: cells + two-space gutters.
    let content_w: usize = w.iter().sum::<usize>() + 2 * (w.len() - 1);
    let count = format!("{} proc{}", rows.len(), if rows.len() == 1 { "" } else { "s" });
    let total = boxd::fit(content_w, "/proc", &count, "");

    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::top(total, "/proc", &count), color::reset(on));
    // Header row (dim).
    {
        let mut line = String::new();
        for (i, h) in HDR.iter().enumerate() {
            if i > 0 {
                line.push_str("  ");
            }
            pad_cell(&mut line, h, w[i], ALIGN[i]);
        }
        let vis = line.chars().count();
        let _ = write!(out, "{}{} {}", color::col(palette::DIM, on), boxd::V, color::reset(on));
        let _ = write!(out, "{}{}{}", color::col(palette::DIM, on), line, color::reset(on));
        for _ in 0..boxd::pad(total, vis) {
            out.put(b" ");
        }
        let _ = write!(out, " {}{}{}\n", color::col(palette::DIM, on), boxd::V, color::reset(on));
    }
    for r in rows {
        let cs = cells(r);
        let mut vis = 0usize;
        let _ = write!(out, "{}{} {}", color::col(palette::DIM, on), boxd::V, color::reset(on));
        for i in 0..9 {
            if i > 0 {
                out.put(b"  ");
                vis += 2;
            }
            let mut cell = String::new();
            pad_cell(&mut cell, cs[i], w[i], ALIGN[i]);
            vis += cell.chars().count();
            let col = match i {
                2 => palette::SLATE,
                3 => state_color(&r.state),
                _ => "",
            };
            if col.is_empty() {
                out.put(cell.as_bytes());
            } else {
                let _ = write!(out, "{}{}{}", color::col(col, on), cell, color::reset(on));
            }
        }
        for _ in 0..boxd::pad(total, vis) {
            out.put(b" ");
        }
        let _ = write!(out, " {}{}{}\n", color::col(palette::DIM, on), boxd::V, color::reset(on));
    }
    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::bottom(total, ""), color::reset(on));
}

/// The Rich realization: a beacon table, PID cells presenting their pids.
fn render_rich(out: &mut io::OutSink, rows: &[PsRow]) {
    use beacon::sink::{Cell, ObjType, Sink, Table};
    let mut t = Table::new("rrllrrrrr").hdr();
    t.push_row(HDR.iter().map(|h| Cell::plain(h)).collect());
    for r in rows {
        let cs = cells(r);
        let mut row: Vec<Cell> = Vec::new();
        row.push(Cell::obj(ObjType::Pid, &r.pid, cs[0]));
        for c in &cs[1..] {
            row.push(Cell::plain(c));
        }
        t.push_row(row);
    }
    let mut sout = coreutils::beacon_gate::SinkOut(out);
    let mut s = Sink::new(&mut sout, beacon::Tier::Rich);
    t.realize(&mut s);
}

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut mode = ColorMode::Auto; // a presentation tool with a working gate
    let mut bmode = beacon::BeaconMode::Auto;
    let mut i = 1;
    while let Some(a) = args.get_str(i) {
        i += 1;
        if a == "--color" {
            mode = ColorMode::Always;
            continue;
        }
        if let Some(when) = a.strip_prefix("--color=") {
            match ColorMode::parse_when(when) {
                Some(m) => mode = m,
                None => return usage::die("ps", &format!("invalid --color value -- '{}'", when)),
            }
            continue;
        }
        if a == "--beacon" {
            bmode = beacon::BeaconMode::Always;
            continue;
        }
        if let Some(when) = a.strip_prefix("--beacon=") {
            match beacon::BeaconMode::parse_when(when) {
                Some(m) => bmode = m,
                None => return usage::die("ps", &format!("invalid --beacon value -- '{}'", when)),
            }
            continue;
        }
        return usage::die("ps", &format!("unexpected operand -- '{}'", a));
    }

    // The emission gate (BEACON.md 12.4); SGR is off inside rich output.
    let rich = coreutils::beacon_gate::resolve(bmode) == beacon::Tier::Rich;
    let on = !rich && mode.resolve(|| libthyla_rs::stdout_is_terminal());

    let rows = match procs::list() {
        Ok(rows) => rows,
        Err(e) => {
            eprintln!("ps: /proc: {}", e);
            return 1;
        }
    };

    let mut out = io::OutSink::new();
    if !on && !rich {
        render_plain(&mut out, &rows);
    } else {
        let styled: Vec<PsRow> = rows.iter().map(styled_row).collect();
        if rich {
            render_rich(&mut out, &styled);
        } else {
            render_box(&mut out, &styled, on);
        }
    }
    out.finish("ps", 0)
}
