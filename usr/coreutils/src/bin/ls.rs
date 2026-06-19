// ls [-laFh1] [--color[=WHEN]] [PATH...] -- list directory contents, the
// Thylacine way (COREUTILS-THYLACINE-DESIGN.md).
//
// Plain `ls`: one name per line, color-coded by kind (dir=slate, exec=green,
// graft=violet, dev=gold) with a classify suffix (`/` `*`). `ls -l` (= ll / la):
// a box framed by the directory path + an item count, columns
// MODE OWNER SIZE REALM QID NAME -- where REALM is the namespace nature
// (fs / dev / graft) and QID is the 9P identity. A GRAFT is an entry whose
// `fstat` fails (a live kernel namespace with no `stat_native`): that failure is
// the signal, so the old ugly `??????` row becomes a first-class `graft` (the
// REALM column + the violet name say so).
//
// Color gate: default ON (the exotic look); `--color=never` drops color AND the
// box for a byte-clean, parseable pipe; `--color=auto` is parked == on until a
// kernel TTY check (SYS_FD_DEVCLASS) lands.

#![no_std]
#![no_main]
#![allow(clippy::write_with_newline)] // a trailing \n in a color-formatted line reads naturally

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::fmt::Write as _;
use coreutils::color::{self, ColorMode};
use coreutils::meta::{self, Kind};
use coreutils::{boxd, palette, size, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Result;
use libthyla_rs::eprintln;
use libthyla_rs::fs::{self, Metadata};
use libthyla_rs::io;

const USAGE: &str = "\
usage: ls [-laFh1] [--color[=WHEN]] [PATH...]
  List directory contents the Thylacine way: names color-coded by kind
  (dir / exec / graft / dev); -l boxes the listing with a REALM + 9P QID
  column. A graft is a live kernel namespace (fstat can't cross it).
  -a  include dotfiles          -l  long (boxed) format
  -h  human-readable sizes       -F  classify (/ dir, * exec)
  -1  one entry per line         --color[=WHEN]  always | never | auto
  --help  show this help

Examples:
  ls                    # names, color-coded by kind
  ls -l                 # long (boxed) with REALM + QID
  ls -la /              # all entries of /
";

// Kind / classify / perms / owner / qid presentation are shared with stat /
// realm / qid via `coreutils::meta`.

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut all = false;
    let mut long = false;
    let mut human = false;
    let mut classify_force = false;
    let mut mode = ColorMode::Always; // exotic by default
    let mut operands: Vec<&str> = Vec::new();
    let mut opts_done = false;

    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        idx += 1;
        if opts_done {
            operands.push(a);
            continue;
        }
        if a == "--" {
            opts_done = true;
            continue;
        }
        if a == "--color" {
            mode = ColorMode::Always;
            continue;
        }
        if let Some(when) = a.strip_prefix("--color=") {
            match ColorMode::parse_when(when) {
                Some(m) => mode = m,
                None => return usage::die("ls", &format!("invalid --color value -- '{}'", when)),
            }
            continue;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'a' => all = true,
                    'l' => long = true,
                    'h' => human = true,
                    'F' => classify_force = true,
                    '1' => {}
                    _ => return usage::die("ls", &format!("invalid option -- '{}'", ch)),
                }
            }
            continue;
        }
        operands.push(a);
    }

    let on = mode.resolve(stdout_is_console);

    // No operand -> the per-Proc cwd (LS-4). Held so the &str outlives the loop.
    let cwd_holder = if operands.is_empty() {
        Some(env::current_dir().unwrap_or_else(|_| String::from("/")))
    } else {
        None
    };
    if let Some(ref c) = cwd_holder {
        operands.push(c.as_str());
    }

    // Partition operands into files (stattable, non-dir) and dirs (a directory,
    // or an unstattable path -- which lists + errors, preserving the old behavior).
    let mut files: Vec<&str> = Vec::new();
    let mut dirs: Vec<&str> = Vec::new();
    for &path in &operands {
        if matches!(fs::metadata(path), Ok(m) if !m.is_dir()) {
            files.push(path);
        } else {
            dirs.push(path);
        }
    }
    let multi = operands.len() > 1;

    let mut status = 0;
    let mut out = io::OutSink::new();
    let mut first = true;

    // File operands first (GNU order). In long mode they share the boxed renderer
    // (titled by the cwd), so `ls -l file` looks like `ls -l dir`.
    if !files.is_empty() {
        if long {
            let here = env::current_dir().unwrap_or_else(|_| String::from("/"));
            let fe: Vec<LongEntry> = files
                .iter()
                .map(|p| LongEntry {
                    display: String::from(*p),
                    path: String::from(*p),
                    rd_dir: false,
                })
                .collect();
            render_long(&mut out, &here, &fe, human, on, classify_force);
        } else {
            for &p in &files {
                emit_name(&mut out, "", p, false, on, classify_force);
                out.put(b"\n");
            }
        }
        first = false;
    }

    // Then directories.
    for &dir in &dirs {
        if multi && !long {
            if !first {
                out.put(b"\n");
            }
            let _ = write!(out, "{}:\n", dir);
        }
        first = false;
        let r = if long {
            list_long_dir(&mut out, dir, all, human, on, classify_force)
        } else {
            list_short_dir(&mut out, dir, all, on, classify_force)
        };
        if let Err(e) = r {
            eprintln!("ls: {}: {}", dir, e);
            status = 1;
        }
    }
    if out.failed() {
        eprintln!("ls: write error");
        return 1;
    }
    status
}

/// `--color=auto` resolution. Reliable TTY detection needs a kernel surface that
/// does NOT exist yet: a console fd (`SYS_CONSOLE_OPEN`) and a pipe fd are both
/// path-less `KOBJ_SPOOR` Devs with no `stat_native`, so `fstat(1)` fails on both
/// and `fd2path(1)` returns 0 for both -- indistinguishable from userspace. So
/// `auto` is color-on for now (the exotic default); use `--color=never` for a
/// clean pipe. The one-line fix when a kernel `SYS_ISATTY` lands
/// (COREUTILS-THYLACINE-DESIGN.md) is to call it here.
fn stdout_is_console() -> bool {
    true
}

/// Read a directory's entries (name + whether readdir called it a directory),
/// filtered + sorted. Shared by the short + long listers.
fn read_entries(dir: &str, all: bool) -> Result<Vec<(String, bool)>> {
    let mut ents: Vec<(String, bool)> = Vec::new();
    for ent in fs::read_dir(dir)? {
        let e = ent?;
        let rd_dir = e.is_dir();
        let name = e.into_file_name();
        if !all && name.starts_with('.') {
            continue;
        }
        ents.push((name, rd_dir));
    }
    ents.sort_unstable_by(|a, b| a.0.cmp(&b.0));
    Ok(ents)
}

/// Plain `ls`: one colored, classified name per line.
fn list_short_dir(
    out: &mut io::OutSink,
    dir: &str,
    all: bool,
    on: bool,
    classify_force: bool,
) -> Result<()> {
    for (name, rd_dir) in read_entries(dir, all)? {
        emit_name(out, dir, &name, rd_dir, on, classify_force);
        out.put(b"\n");
    }
    Ok(())
}

/// A single colored, classified name (short mode). When color is off and `-F`
/// is not set, this is the bare name -- byte-clean for a pipe.
fn emit_name(out: &mut io::OutSink, dir: &str, name: &str, rd_dir: bool, on: bool, classify_force: bool) {
    if !on && !classify_force {
        out.put(name.as_bytes());
        return;
    }
    let md = fs::metadata(join(dir, name)).ok();
    let kind = meta::classify(rd_dir, &md);
    let _ = write!(
        out,
        "{}{}{}{}",
        color::col(kind.color(), on),
        name,
        kind.suffix(),
        color::reset(on)
    );
}

/// One assembled long-format row: the plain left columns + the name (colored at
/// emit). `name`/`suffix` are kept separate so the box pad is computed on the
/// PLAIN visible width while the name carries its color.
struct Row {
    prefix: String,
    name: String,
    suffix: &'static str,
    color: &'static str,
}

/// An entry to render in a long listing: its display name, its full path (to
/// fstat), and whether `readdir` called it a directory (graft detection). A
/// directory's entries and explicit file operands both become these.
struct LongEntry {
    display: String,
    path: String,
    rd_dir: bool,
}

/// `ls -l` over a directory: build the entry list from readdir, then render.
fn list_long_dir(
    out: &mut io::OutSink,
    dir: &str,
    all: bool,
    human: bool,
    on: bool,
    classify_force: bool,
) -> Result<()> {
    let entries: Vec<LongEntry> = read_entries(dir, all)?
        .into_iter()
        .map(|(name, rd_dir)| LongEntry {
            path: join(dir, &name),
            display: name,
            rd_dir,
        })
        .collect();
    render_long(out, dir, &entries, human, on, classify_force);
    Ok(())
}

/// Render `entries` as a long listing titled `title`. Color on -> the boxed
/// presentation with the REALM + QID columns; color off -> the same columns
/// space-separated with no box / header / color (parseable + byte-clean). Shared
/// by a directory listing and explicit file operands, so `ls -l file` gets the
/// same look as `ls -l dir`.
fn render_long(
    out: &mut io::OutSink,
    title: &str,
    entries: &[LongEntry],
    human: bool,
    on: bool,
    classify_force: bool,
) {
    // Cells per entry (and the widths they drive).
    let mut mode_s: Vec<String> = Vec::new();
    let mut owner_s: Vec<String> = Vec::new();
    let mut size_s: Vec<String> = Vec::new();
    let mut realm_s: Vec<&'static str> = Vec::new();
    let mut qid_s: Vec<String> = Vec::new();
    let mut name_s: Vec<String> = Vec::new();
    let mut suffix_s: Vec<&'static str> = Vec::new();
    let mut color_s: Vec<&'static str> = Vec::new();

    for e in entries {
        let md = fs::metadata(&e.path).ok();
        let kind = meta::classify(e.rd_dir, &md);
        match &md {
            Some(m) => {
                mode_s.push(meta::perms_string(m));
                owner_s.push(meta::owner(m.uid()));
                size_s.push(size_str(kind, m, human));
                qid_s.push(meta::qid_compact(m));
            }
            None => {
                // A graft (or an unstattable entry): no perms/owner/size/qid.
                mode_s.push(String::from("--"));
                owner_s.push(String::from("-"));
                size_s.push(String::from("-"));
                qid_s.push(String::from("-"));
            }
        }
        realm_s.push(kind.realm());
        name_s.push(e.display.clone());
        suffix_s.push(kind.suffix());
        color_s.push(kind.color());
    }

    // Column widths (>= the header label widths).
    let mw = 10usize; // perms are 10
    let ow = owner_s.iter().map(|s| s.chars().count()).max().unwrap_or(0).max(5);
    let sw = size_s.iter().map(|s| s.chars().count()).max().unwrap_or(0).max(4);
    let rw = 5usize; // "graft" / "REALM"
    let qw = qid_s.iter().map(|s| s.chars().count()).max().unwrap_or(0).max(3);

    // Build rows (header first).
    let mut rows: Vec<Row> = Vec::new();
    rows.push(Row {
        prefix: row_prefix("MODE", "OWNER", "SIZE", "REALM", "QID", mw, ow, sw, rw, qw),
        name: String::from("NAME"),
        suffix: "",
        color: palette::DIM,
    });
    for i in 0..entries.len() {
        rows.push(Row {
            prefix: row_prefix(&mode_s[i], &owner_s[i], &size_s[i], realm_s[i], &qid_s[i], mw, ow, sw, rw, qw),
            name: name_s[i].clone(),
            suffix: suffix_s[i],
            color: color_s[i],
        });
    }

    if !on {
        // Plain parseable long format: the data rows only (no box / header /
        // color), suffixes only under -F. The pipe-clean discipline.
        for r in rows.iter().skip(1) {
            if classify_force {
                let _ = write!(out, "{}{}{}\n", r.prefix, r.name, r.suffix);
            } else {
                let _ = write!(out, "{}{}\n", r.prefix, r.name);
            }
        }
        return;
    }

    // Box geometry.
    let content_w = rows
        .iter()
        .map(|r| r.prefix.chars().count() + r.name.chars().count() + r.suffix.chars().count())
        .max()
        .unwrap_or(0);
    let count = format!("{} item{}", entries.len(), if entries.len() == 1 { "" } else { "s" });
    let total = boxd::fit(content_w, title, &count, "");

    // Emit: top border, header (dim), rows (name colored), bottom rule. The
    // realm column + the violet name carry the graft meaning -- no legend.
    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::top(total, title, &count), color::reset(on));
    for (i, r) in rows.iter().enumerate() {
        emit_row(out, total, r, on, i == 0);
    }
    let _ = write!(out, "{}{}{}\n", color::col(palette::DIM, on), boxd::bottom(total, ""), color::reset(on));
}

/// Emit one boxed content row: `│ {prefix}{name}{suffix}{pad} │`. The header
/// row colors the whole content dim; an entry row colors only the name.
fn emit_row(out: &mut io::OutSink, total: usize, r: &Row, on: bool, header: bool) {
    let vis = r.prefix.chars().count() + r.name.chars().count() + r.suffix.chars().count();
    let pad = boxd::pad(total, vis);
    let _ = write!(out, "{}{} {}", color::col(palette::DIM, on), boxd::V, color::reset(on));
    if header {
        let _ = write!(out, "{}{}{}{}", color::col(palette::DIM, on), r.prefix, r.name, color::reset(on));
    } else {
        let _ = write!(
            out,
            "{}{}{}{}{}",
            r.prefix,
            color::col(r.color, on),
            r.name,
            r.suffix,
            color::reset(on)
        );
    }
    for _ in 0..pad {
        out.put(b" ");
    }
    let _ = write!(out, " {}{}{}\n", color::col(palette::DIM, on), boxd::V, color::reset(on));
}

/// The fixed-width left columns of a long row (everything before the name),
/// ending with the two-space gutter before the name.
#[allow(clippy::too_many_arguments)]
fn row_prefix(
    mode: &str,
    owner: &str,
    sizev: &str,
    realm: &str,
    qid: &str,
    mw: usize,
    ow: usize,
    sw: usize,
    rw: usize,
    qw: usize,
) -> String {
    format!(
        "{:<mw$}  {:<ow$}  {:>sw$}  {:<rw$}  {:<qw$}  ",
        mode,
        owner,
        sizev,
        realm,
        qid,
        mw = mw,
        ow = ow,
        sw = sw,
        rw = rw,
        qw = qw
    )
}

/// The SIZE column: bytes (or human) for a regular/executable file; `-` for a
/// directory / device (size is not meaningful in this view).
fn size_str(kind: Kind, m: &Metadata, human: bool) -> String {
    match kind {
        Kind::File | Kind::Exec => {
            if human {
                size::human(m.len())
            } else {
                format!("{}", m.len())
            }
        }
        _ => String::from("-"),
    }
}

fn join(dir: &str, name: &str) -> String {
    if dir.is_empty() {
        return String::from(name);
    }
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
