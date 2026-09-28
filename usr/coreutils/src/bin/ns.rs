// ns [--color[=WHEN]] [pid] -- print a process's namespace (its territory mount
// list), the Plan 9 `ns` tool, the Thylacine way. Reads /proc/<pid>/ns, which
// the kernel renders as one "mount <mountpoint> <source>[ <suffix>]..." line per
// mount entry plus a trailing "binds: <N>" count (devproc -> territory_format_ns;
// #66, I-33), parsed by coreutils::nsmount.
//
// The mountpoint column is the namespace name the directory was mounted onto
// (a Spoor.path, #66a); the source is the mounted tree's name, or a Plan 9
// device spec "#<dc>" (e.g. "#9"=9P, "#s"=srv, "#p"=proc) when the source is a
// device root with no namespace name. We colorize + box the listing and add a
// REALM column derived from the device char -- or `remote` for an entry the
// kernel marks ` remote` (a 9P session declared remote, HAUL-DESIGN 4.8) -- and
// a FLAGS column for the other suffixes. A presentation tool -> color on the
// console (auto); --color=never passes the raw kernel text through.
//
// `ns` with no operand shows the caller's own namespace (Plan 9's default);
// `ns 0` shows kproc's, the system root.

#![no_std]
#![no_main]
#![allow(clippy::write_with_newline)] // a trailing \n in a color-formatted line reads naturally

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use core::fmt::Write as _;
use coreutils::color::{self, ColorMode};
use coreutils::nsmount::{self, Line, Mount};
use coreutils::{boxd, meta, palette, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

const USAGE: &str = "\
usage: ns [--color[=WHEN]] [pid]
  Print a process's namespace -- its territory mount list (mountpoint,
  source, the source's realm, and the entry's flags). No pid shows the
  caller's own namespace; pid 0 is the system root.
  --color[=WHEN]  colorize: always | never (raw kernel text) | auto (default)
  --help          show this help

Examples:
  ns                    # this shell's namespace
  ns 0                  # the system root namespace
  ns 1                  # a process's mount list
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

/// `(realm, color)` for a mount entry: `remote` when the kernel marks it so;
/// else a `#<dc>` device spec maps to its realm by the device char, and a
/// namespace-name source is a plain fs subtree.
fn entry_realm(m: &Mount) -> (&'static str, &'static str) {
    if m.remote {
        return ("remote", palette::EMBER);
    }
    source_realm(m.source)
}

fn source_realm(src: &str) -> (&'static str, &'static str) {
    match src.strip_prefix('#').and_then(|s| s.chars().next()) {
        Some('9') => ("9p", palette::SLATE),
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
    let mut mode = ColorMode::Auto; // color iff console (the H-1 SYS_FD_DEVCLASS unification)
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
        pid = unsafe { libthyla_rs::t_getpid() }; // default: the caller's namespace
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

    let mut out = io::OutSink::new();
    // --color=never: pass the raw kernel rendering through, byte-clean.
    if !on {
        out.put(&data);
        return out.finish("ns", 0);
    }

    let text = core::str::from_utf8(&data).unwrap_or("");
    let mut mounts: Vec<Mount> = Vec::new();
    let mut binds: Option<u64> = None;
    let mut root_pheno = false;
    for line in text.lines() {
        match nsmount::parse_line(line) {
            Line::Mount(m) => mounts.push(m),
            Line::Binds(n) => binds = Some(n),
            Line::RootPheno => root_pheno = true,
            Line::Other(_) => {}
        }
    }

    // Defensive: if the kernel's format ever drifts from "mount <mp> <src>" and
    // we parse nothing while the text clearly has mounts, pass the raw text
    // through rather than show an empty box (never lose the user's data).
    if mounts.is_empty() && text.contains("mount") {
        out.put(&data);
        return out.finish("ns", 0);
    }

    render(&mut out, pid, &mounts, binds, root_pheno, on);
    out.finish("ns", 0)
}

/// The FLAGS cell: the suffixes the kernel rendered other than ` remote` (which
/// is the REALM), in the kernel's order (noexec, pheno-linux, covered), then any
/// this tool does not know, as written.
fn flags_cell(m: &Mount) -> String {
    let mut f: Vec<&str> = Vec::new();
    if m.noexec {
        f.push("noexec");
    }
    if m.pheno_linux {
        f.push("pheno-linux");
    }
    if m.covered {
        f.push("covered");
    }
    f.extend(m.unknown.iter().copied());
    if f.is_empty() {
        String::from("-")
    } else {
        f.join(",")
    }
}

/// Render the boxed namespace view: MOUNTPOINT / SOURCE / REALM / FLAGS, each
/// cell colored by kind (mountpoint slate, source + realm by the realm, flags
/// dim). A namespace-level `root: pheno-linux` rides the bottom rule.
fn render(out: &mut io::OutSink, pid: i64, mounts: &[Mount], binds: Option<u64>, root_pheno: bool, on: bool) {
    let realms: Vec<(&'static str, &'static str)> = mounts.iter().map(entry_realm).collect();
    let flags: Vec<String> = mounts.iter().map(flags_cell).collect();
    let mpw = mounts.iter().map(|m| m.point.chars().count()).max().unwrap_or(0).max(10); // "MOUNTPOINT"
    let srcw = mounts.iter().map(|m| m.source.chars().count()).max().unwrap_or(0).max(6); // "SOURCE"
    let rw = realms.iter().map(|(r, _)| r.chars().count()).max().unwrap_or(0).max(5); // "REALM"
    let fw = flags.iter().map(|f| f.chars().count()).max().unwrap_or(0).max(5); // "FLAGS"
    let content_w = mpw + 2 + srcw + 2 + rw + 2 + fw;

    let title = format!("namespace of pid {}", pid);
    // The kernel writes `binds:` only after a whole list (#66b); without it
    // the list was cut, and the count cell says so instead of a zero.
    let count = match binds {
        Some(n) => format!("{} bind{}", n, if n == 1 { "" } else { "s" }),
        None => String::from(meta::MOUNT_LIST_CUT),
    };
    let foot = if root_pheno { "root: pheno-linux" } else { "" };
    let total = boxd::fit(content_w, &title, &count, foot);

    // top border (dim)
    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::top(total, &title, &count), color::reset(on));
    // header row (dim)
    let header = format!(
        "{:<mpw$}  {:<srcw$}  {:<rw$}  {:<fw$}",
        "MOUNTPOINT", "SOURCE", "REALM", "FLAGS",
        mpw = mpw, srcw = srcw, rw = rw, fw = fw
    );
    emit_row(out, total, &header, on);
    // entries
    for ((m, (realm, rcolor)), fl) in mounts.iter().zip(&realms).zip(&flags) {
        let body = format!(
            "{}{:<mpw$}{}  {}{:<srcw$}{}  {}{:<rw$}{}  {}{:<fw$}{}",
            color::col(palette::SLATE, on), m.point, color::reset(on),
            color::col(rcolor, on), m.source, color::reset(on),
            color::col(rcolor, on), realm, color::reset(on),
            color::col(palette::DIM, on), fl, color::reset(on),
            mpw = mpw, srcw = srcw, rw = rw, fw = fw
        );
        emit_colored_row(out, total, content_w, &body, on);
    }
    // bottom rule (dim)
    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::bottom(total, foot), color::reset(on));
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

/// `--color=auto`: stdout is the interactive console iff its Dev class is
/// `'c'` (`SYS_FD_DEVCLASS`; H-1 closed the long-parked `true` stub).
fn stdout_is_console() -> bool {
    libthyla_rs::stdout_is_terminal()
}
