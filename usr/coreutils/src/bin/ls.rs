// ls [-laFh1] [--color[=WHEN]] [--beacon=WHEN] [PATH...] -- list directory
// contents, the Thylacine way (COREUTILS-THYLACINE-DESIGN.md).
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
// Color gate: default AUTO since H-1 -- SYS_FD_DEVCLASS answers the long-
// parked TTY question (dc 'c' == the interactive console), so `ls | cat` is
// byte-clean while the console keeps the exotic look. `--color=always` forces.
//
// Beacon (docs/BEACON.md): at the Rich tier the listing is emitted as
// semantic frames -- names carry `obj type=path` with the cleaned absolute
// ref, and `-l` realizes a beacon `table` (plain-aligned payload + frames,
// NO box -- the renderer restyles). SGR is off inside rich-structured output
// (the renderer stylesheet owns typography). The cells/never realizations
// above are untouched at every other tier.

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
usage: ls [-laFh1] [--color[=WHEN]] [--beacon=WHEN] [PATH...]
  List directory contents the Thylacine way: names color-coded by kind
  (dir / exec / graft / dev); -l boxes the listing with a REALM + 9P QID
  column. A graft is a live kernel namespace (fstat can't cross it).
  -a  include dotfiles          -l  long (boxed) format
  -h  human-readable sizes       -F  classify (/ dir, * exec)
  -1  one entry per line         --color[=WHEN]  always | never | auto
  --beacon=WHEN  semantic markup: auto (default) | always | never
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
    // AUTO since H-1: the console gets the exotic look, a pipe gets clean
    // bytes -- SYS_FD_DEVCLASS finally answers which one stdout is.
    let mut mode = ColorMode::Auto;
    let mut bmode = beacon::BeaconMode::Auto;
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
        if a == "--beacon" {
            bmode = beacon::BeaconMode::Always;
            continue;
        }
        if let Some(when) = a.strip_prefix("--beacon=") {
            match beacon::BeaconMode::parse_when(when) {
                Some(m) => bmode = m,
                None => return usage::die("ls", &format!("invalid --beacon value -- '{}'", when)),
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

    // The emission gate (BEACON.md 12.4). At Rich, SGR goes OFF inside the
    // structured output -- the renderer stylesheet owns typography there.
    let rich = coreutils::beacon_gate::resolve(bmode) == beacon::Tier::Rich;
    let on = !rich && mode.resolve(stdout_is_console);

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
            render_long(&mut out, &here, &fe, human, on, classify_force, rich);
        } else {
            for &p in &files {
                emit_name(&mut out, "", p, false, on, classify_force, rich);
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
            list_long_dir(&mut out, dir, all, human, on, classify_force, rich)
        } else {
            list_short_dir(&mut out, dir, all, on, classify_force, rich)
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

/// `--color=auto` resolution: stdout is the interactive console iff its Dev
/// class is `'c'` (`SYS_FD_DEVCLASS`; the kernel normalizes the walked
/// `/dev/cons` leaf too -- docs/SYS-FD-DEVCLASS-SPEC.md AS-BUILT). H-1 closed
/// the long-parked `true` stub: a pipe / file / closed fd resolves color-off.
fn stdout_is_console() -> bool {
    libthyla_rs::stdout_is_terminal()
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
    rich: bool,
) -> Result<()> {
    for (name, rd_dir) in read_entries(dir, all)? {
        emit_name(out, dir, &name, rd_dir, on, classify_force, rich);
        out.put(b"\n");
    }
    Ok(())
}

/// A single colored, classified name (short mode). When color is off and `-F`
/// is not set, this is the bare name -- byte-clean for a pipe. At Rich the
/// SAME plain bytes are bracketed by an `obj type=path` frame (the cleaned
/// absolute ref); the classify suffix stays OUTSIDE the frame (it is
/// presentation, not part of the name), so strip() recovers the plain line.
fn emit_name(out: &mut io::OutSink, dir: &str, name: &str, rd_dir: bool, on: bool, classify_force: bool, rich: bool) {
    if rich {
        {
            let mut sout = coreutils::beacon_gate::SinkOut(out);
            let mut s = beacon::sink::Sink::new(&mut sout, beacon::Tier::Rich);
            match coreutils::path::abs(&join(dir, name)) {
                Some(r) => s.obj(beacon::sink::ObjType::Path, &r, name),
                None => s.text(name),
            }
        }
        if classify_force {
            let md = fs::metadata(join(dir, name)).ok();
            out.put(meta::classify(rd_dir, &md).suffix().as_bytes());
        }
        return;
    }
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
#[allow(clippy::too_many_arguments)]
fn list_long_dir(
    out: &mut io::OutSink,
    dir: &str,
    all: bool,
    human: bool,
    on: bool,
    classify_force: bool,
    rich: bool,
) -> Result<()> {
    let entries: Vec<LongEntry> = read_entries(dir, all)?
        .into_iter()
        .map(|(name, rd_dir)| LongEntry {
            path: join(dir, &name),
            display: name,
            rd_dir,
        })
        .collect();
    render_long(out, dir, &entries, human, on, classify_force, rich);
    Ok(())
}

/// Render `entries` as a long listing titled `title`. Color on -> the boxed
/// presentation with the REALM + QID columns; color off -> the same columns
/// space-separated with no box / header / color (parseable + byte-clean); Rich
/// -> a beacon `table` (header + plain-aligned payload + frames, no box --
/// the renderer restyles) with `obj type=path` on the name cells. Shared
/// by a directory listing and explicit file operands, so `ls -l file` gets the
/// same look as `ls -l dir`.
#[allow(clippy::too_many_arguments)]
fn render_long(
    out: &mut io::OutSink,
    title: &str,
    entries: &[LongEntry],
    human: bool,
    on: bool,
    classify_force: bool,
    rich: bool,
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

    if rich {
        // The beacon table realization: same columns, widths from content
        // (Table's own math), the name cells presenting their objects. The
        // classify suffix is dropped here -- the REALM column classifies.
        use beacon::sink::{Cell, ObjType, Sink, Table};
        let mut t = Table::new("llrlll").hdr();
        t.push_row(alloc::vec![
            Cell::plain("MODE"),
            Cell::plain("OWNER"),
            Cell::plain("SIZE"),
            Cell::plain("REALM"),
            Cell::plain("QID"),
            Cell::plain("NAME"),
        ]);
        for i in 0..entries.len() {
            let name_cell = match coreutils::path::abs(&entries[i].path) {
                Some(r) => Cell::obj(ObjType::Path, &r, &name_s[i]),
                None => Cell::plain(&name_s[i]),
            };
            t.push_row(alloc::vec![
                Cell::plain(&mode_s[i]),
                Cell::plain(&owner_s[i]),
                Cell::plain(&size_s[i]),
                Cell::plain(realm_s[i]),
                Cell::plain(&qid_s[i]),
                name_cell,
            ]);
        }
        let mut sout = coreutils::beacon_gate::SinkOut(out);
        let mut s = Sink::new(&mut sout, beacon::Tier::Rich);
        t.realize(&mut s);
        return;
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
