// ns [--color[=WHEN]] [pid] -- print a process's namespace (its territory mount
// list), the Plan 9 `ns` tool, the Thylacine way. Reads /proc/<pid>/ns, which
// the kernel renders as one "mount <mountpoint> <source>" line per mount entry
// plus a trailing "binds: <N>" count (devproc -> territory_format_ns; #66, I-33).
//
// The mountpoint column is the namespace name the directory was mounted onto
// (a Spoor.path, #66a); the source is the mounted tree's name, or a Plan 9
// device spec "#<dc>" (e.g. "#9"=9P/disk, "#s"=srv, "#p"=proc) when the source
// is a device root with no namespace name. We colorize + box the listing and add
// a REALM column derived from the device char -- the precise realm, available NOW
// from the kernel's "#<dc>" text (no SYS_FD_DEVCLASS needed). A presentation tool
// -> color on by default; --color=never passes the raw kernel text through.
//
// `ns` with no operand shows kproc's namespace (pid 0 -- the system root).

#![no_std]
#![no_main]
#![allow(clippy::write_with_newline)] // a trailing \n in a color-formatted line reads naturally

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::format;
use alloc::vec::Vec;

use core::fmt::Write as _;
use coreutils::color::{self, ColorMode};
use coreutils::{boxd, palette, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

const USAGE: &str = "\
usage: ns [--color[=WHEN]] [pid]
  Print a process's namespace -- its territory mount list (mountpoint,
  source, and the source's realm). No pid shows the system root (pid 0).
  --color[=WHEN]  colorize: always (default) | never (raw kernel text)
  --help          show this help

Examples:
  ns                    # the system root namespace (pid 0)
  ns 1                  # a process's mount list
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

/// `(realm, color)` for a mount source: a `#<dc>` device spec maps to its realm
/// by the device char; a namespace-name source is a plain fs subtree.
fn source_realm(src: &str) -> (&'static str, &'static str) {
    match src.strip_prefix('#').and_then(|s| s.chars().next()) {
        Some('9') => ("disk", palette::SLATE),
        Some('r') | Some('M') => ("boot", palette::SLATE),
        Some('p') => ("proc", palette::VIOLET),
        Some('s') => ("srv", palette::VIOLET),
        Some('H') => ("hw", palette::VIOLET),
        Some('n') => ("notes", palette::VIOLET),
        Some('d') => ("dev", palette::GOLD),
        Some('c') | Some('C') => ("cons", palette::GOLD),
        Some(_) => ("dev", palette::GOLD),
        None => ("fs", palette::SLATE), // a namespace-name source subtree
    }
}

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut mode = ColorMode::Always; // presentation tool
    let mut pid: i64 = -1;
    let mut opts_done = false;
    let mut i = 1;
    while let Some(a) = args.get_str(i) {
        i += 1;
        if !opts_done {
            if a == "--" {
                opts_done = true;
                continue;
            }
            if a == "--color" {
                mode = ColorMode::Always;
                continue;
            }
            if let Some(w) = a.strip_prefix("--color=") {
                match ColorMode::parse_when(w) {
                    Some(m) => mode = m,
                    None => return usage::die("ns", &format!("invalid --color value -- '{}'", w)),
                }
                continue;
            }
            if a.starts_with('-') && a != "-" && a.len() > 1 {
                return usage::die("ns", &format!("invalid option -- '{}'", a));
            }
        }
        if pid >= 0 {
            return usage::die("ns", "too many operands");
        }
        match parse_pid(a) {
            Some(v) => pid = v,
            None => return usage::die("ns", "invalid pid operand"),
        }
    }
    if pid < 0 {
        pid = 0; // default: the system root namespace
    }

    let on = mode.resolve(stdout_is_console);
    let path = format!("/proc/{}/ns", pid);
    let data = match File::open(&path).and_then(|mut f| io::slurp(&mut f)) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("ns: {}: {}", path, e);
            return 1;
        }
    };

    // --color=never: pass the raw kernel rendering through, byte-clean.
    if !on {
        io::out(&data);
        return 0;
    }

    let text = core::str::from_utf8(&data).unwrap_or("");
    let mut mounts: Vec<(&str, &str)> = Vec::new();
    let mut binds = 0usize;
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("mount ") {
            let mut it = rest.split_whitespace();
            let mp = it.next().unwrap_or("");
            let src = it.next().unwrap_or("");
            mounts.push((mp, src));
        } else if let Some(n) = line.strip_prefix("binds: ") {
            binds = n.trim().parse().unwrap_or(mounts.len());
        }
    }

    // Defensive: if the kernel's format ever drifts from "mount <mp> <src>" and
    // we parse nothing while the text clearly has mounts, pass the raw text
    // through rather than show an empty box (never lose the user's data).
    if mounts.is_empty() && text.contains("mount") {
        io::out(&data);
        return 0;
    }

    let mut out = io::OutSink::new();
    render(&mut out, pid, &mounts, binds, on);
    if out.failed() {
        eprintln!("ns: write error");
        return 1;
    }
    0
}

/// Render the boxed namespace view: MOUNTPOINT / SOURCE / REALM, each cell
/// colored by kind (mountpoint slate, source + realm by the device realm).
fn render(out: &mut io::OutSink, pid: i64, mounts: &[(&str, &str)], binds: usize, on: bool) {
    let realms: Vec<(&'static str, &'static str)> = mounts.iter().map(|(_, s)| source_realm(s)).collect();
    let mpw = mounts.iter().map(|(m, _)| m.chars().count()).max().unwrap_or(0).max(10); // "MOUNTPOINT"
    let srcw = mounts.iter().map(|(_, s)| s.chars().count()).max().unwrap_or(0).max(6); // "SOURCE"
    let rw = realms.iter().map(|(r, _)| r.chars().count()).max().unwrap_or(0).max(5); // "REALM"
    let content_w = mpw + 2 + srcw + 2 + rw;

    let title = format!("namespace of pid {}", pid);
    let count = format!("{} bind{}", binds, if binds == 1 { "" } else { "s" });
    let total = boxd::fit(content_w, &title, &count, "");

    // top border (dim)
    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::top(total, &title, &count), color::reset(on));
    // header row (dim)
    let header = format!("{:<mpw$}  {:<srcw$}  {:<rw$}", "MOUNTPOINT", "SOURCE", "REALM", mpw = mpw, srcw = srcw, rw = rw);
    emit_row(out, total, &header, on);
    // entries
    for ((mp, src), (realm, rcolor)) in mounts.iter().zip(&realms) {
        let body = format!(
            "{}{:<mpw$}{}  {}{:<srcw$}{}  {}{:<rw$}{}",
            color::col(palette::SLATE, on), mp, color::reset(on),
            color::col(rcolor, on), src, color::reset(on),
            color::col(rcolor, on), realm, color::reset(on),
            mpw = mpw, srcw = srcw, rw = rw
        );
        emit_colored_row(out, total, content_w, &body, on);
    }
    // bottom rule (dim)
    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::bottom(total, ""), color::reset(on));
}

/// A header (all-dim) content row whose PLAIN width is the field width.
fn emit_row(out: &mut io::OutSink, total: usize, plain: &str, on: bool) {
    let vis = plain.chars().count();
    let pad = boxd::pad(total, vis);
    let _ = write!(out, "{}{} {}", color::col(palette::DIM, on), boxd::V, color::reset(on));
    let _ = write!(out, "{}{}{}", color::col(palette::DIM, on), plain, color::reset(on));
    for _ in 0..pad {
        out.put(b" ");
    }
    let _ = write!(out, " {}{}{}\n", color::col(palette::DIM, on), boxd::V, color::reset(on));
}

/// An entry row: `body` already carries its color spans; `content_w` is its PLAIN
/// visible width (the caller knows it from the column widths).
fn emit_colored_row(out: &mut io::OutSink, total: usize, content_w: usize, body: &str, on: bool) {
    let pad = boxd::pad(total, content_w);
    let _ = write!(out, "{}{} {}", color::col(palette::DIM, on), boxd::V, color::reset(on));
    out.put(body.as_bytes());
    for _ in 0..pad {
        out.put(b" ");
    }
    let _ = write!(out, " {}{}{}\n", color::col(palette::DIM, on), boxd::V, color::reset(on));
}

// Parse a non-negative decimal pid. Rejects empty / non-digit / overflow.
fn parse_pid(s: &str) -> Option<i64> {
    if s.is_empty() {
        return None;
    }
    let mut v: i64 = 0;
    for b in s.bytes() {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as i64)?;
    }
    Some(v)
}

/// `--color=auto` stub (no cooked-mode TTY check yet; see the SYS_FD_DEVCLASS
/// spec). True so an explicit `--color=auto` colorizes.
fn stdout_is_console() -> bool {
    true
}
