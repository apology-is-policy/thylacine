// ps [--color[=WHEN]] [--beacon=WHEN] -- list processes, the Thylacine way.
//
// One atomic read of /ctl/procs (the kernel renders the whole table under
// g_proc_table_lock -- no readdir race), columns PID PPID NAME STATE THREADS
// PAGES CHILDREN CPU_NS. Styled (a presentation tool, color auto): a boxed
// listing with CPU humanized (ns -> ms/s) and the state colored (ALIVE green,
// ZOMBIE ember, STOPPED gold). Color off: the kernel text passes
// through VERBATIM (parseable, byte-clean -- the ns/pelt discipline). If any
// row fails to parse (a kernel format change), the whole output degrades to
// the verbatim pass-through rather than a partial table.
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
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

const USAGE: &str = "\
usage: ps [--color[=WHEN]] [--beacon=WHEN]
  List processes (one atomic /ctl/procs snapshot): PID, parent, name, state,
  threads, resident pages, children, CPU time. Color off passes the kernel
  text through verbatim (parseable).
  --color[=WHEN]  colorize: always | never | auto (default)
  --beacon=WHEN   semantic markup: auto (default) | always | never
  --help  show this help

Examples:
  ps                    # boxed, colored on the console; plain in a pipe
  ps --color=never      # the raw kernel table, byte-clean
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
    children: String,
    cpu: String, // humanized
}

/// ns -> a compact human figure: "<n>ms" under 10s, else "<n>s".
fn cpu_str(ns_text: &str) -> Option<String> {
    let ns: u64 = ns_text.parse().ok()?;
    let ms = ns / 1_000_000;
    Some(if ms < 10_000 {
        format!("{}ms", ms)
    } else {
        format!("{}s", ms / 1000)
    })
}

/// Parse one /ctl/procs data row. The NAME column is rejoined from the middle
/// fields so a (hypothetical) spaced name cannot shear the numeric columns.
fn parse_row(line: &str) -> Option<PsRow> {
    let f: Vec<&str> = line.split_whitespace().collect();
    if f.len() < 8 {
        return None;
    }
    let n = f.len();
    Some(PsRow {
        pid: String::from(f[0]),
        ppid: String::from(f[1]),
        name: f[2..n - 5].join(" "),
        state: String::from(f[n - 5]),
        threads: String::from(f[n - 4]),
        pages: String::from(f[n - 3]),
        children: String::from(f[n - 2]),
        cpu: cpu_str(f[n - 1])?,
    })
}

/// The kernel's STATE vocabulary (devctl procs_state_name): ALIVE / ZOMBIE /
/// STOPPED (job-stop) / INVALID / "?".
fn state_color(state: &str) -> &'static str {
    match state {
        "ALIVE" => palette::GREEN,
        "ZOMBIE" => palette::EMBER,
        "STOPPED" => palette::GOLD,
        _ => palette::FG,
    }
}

const HDR: [&str; 8] = ["PID", "PPID", "NAME", "STATE", "THR", "PAGES", "KIDS", "CPU"];

/// Column cells for one row, header-order.
fn cells(r: &PsRow) -> [&str; 8] {
    [&r.pid, &r.ppid, &r.name, &r.state, &r.threads, &r.pages, &r.children, &r.cpu]
}

/// r/l alignment per column: numerics right, NAME/STATE left.
const ALIGN: [u8; 8] = [b'r', b'r', b'l', b'l', b'r', b'r', b'r', b'r'];

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
    let mut w = [0usize; 8];
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
    let total = boxd::fit(content_w, "/ctl/procs", &count, "");

    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::top(total, "/ctl/procs", &count), color::reset(on));
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
        for i in 0..8 {
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
    let mut t = Table::new("rrllrrrr").hdr();
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

    let raw = match File::open("/ctl/procs").and_then(|mut f| io::slurp(&mut f)) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("ps: /ctl/procs: {}", e);
            return 1;
        }
    };

    let mut out = io::OutSink::new();
    if !on && !rich {
        // The verbatim pass-through (parseable; the raw CPU_NS is here).
        out.put(&raw);
    } else {
        let text = String::from_utf8_lossy(&raw);
        let mut rows: Vec<PsRow> = Vec::new();
        let mut ok = true;
        for line in text.lines().skip(1) {
            if line.is_empty() {
                continue;
            }
            match parse_row(line) {
                Some(r) => rows.push(r),
                None => {
                    ok = false;
                    break;
                }
            }
        }
        if !ok {
            // A row this tool cannot parse (kernel format drift): degrade to
            // the verbatim text rather than render a partial table.
            out.put(&raw);
        } else if rich {
            render_rich(&mut out, &rows);
        } else {
            render_box(&mut out, &rows, on);
        }
    }
    if out.failed() {
        eprintln!("ps: write error");
        return 1;
    }
    0
}
