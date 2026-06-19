// pelt [-ad] [-L N] [--color[=WHEN]] [PATH...] -- a realm-colored directory
// tree that STOPS at graft boundaries.
//
// Like `tree`, but Thylacine-shaped: each entry is colored by its kind (dir
// slate, exec green, dev gold, graft violet) and, crucially, a GRAFT -- a live
// kernel namespace mount (an entry readdir calls a directory but fstat cannot
// cross) -- is shown and marked but NEVER descended. So `pelt /` maps the disk
// tree without wandering into /srv, /proc, /dev and trying to walk a live
// kernel namespace as if it were disk.
//
// Naming: the branch glyphs (the striped │ ├ └ rails running down the listing)
// are the thylacine's pelt -- its stripes.
//
// Presentation tool -> color ON by default; --color=never drops color but keeps
// the ASCII rails + the `(graft)` marker (the structure is the output).

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
use coreutils::{palette, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs;
use libthyla_rs::{eprintln, io};

const USAGE: &str = "\
usage: pelt [-ad] [-L N] [--color[=WHEN]] [PATH...]
  Print a directory tree, colored by kind, that stops at graft boundaries
  (a graft is a live kernel namespace -- shown + marked, never descended).
  -a      include dotfiles
  -d      directories only
  -L N    descend at most N levels
  --color[=WHEN]  colorize: always (default) | never | auto
  --help  show this help

Examples:
  pelt                  # tree of the current directory
  pelt -L 2 /           # the disk root, two levels deep
  pelt -d ~             # just the directory skeleton
";

struct Counts {
    dirs: usize,
    files: usize,
    grafts: usize,
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut all = false;
    let mut dirs_only = false;
    let mut max_depth = usize::MAX;
    let mut mode = ColorMode::Always; // presentation tool
    let mut operands: Vec<&str> = Vec::new();
    let mut opts_done = false;

    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        idx += 1;
        if !opts_done {
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
                    None => return usage::die("pelt", &format!("invalid --color value -- '{}'", when)),
                }
                continue;
            }
            if a == "-L" {
                match args.get_str(idx).and_then(|s| s.parse::<usize>().ok()) {
                    Some(n) => {
                        max_depth = n;
                        idx += 1;
                        continue;
                    }
                    None => return usage::die("pelt", "-L requires a number"),
                }
            }
            if a.starts_with('-') && a != "-" && a.len() > 1 {
                for ch in a[1..].chars() {
                    match ch {
                        'a' => all = true,
                        'd' => dirs_only = true,
                        _ => return usage::die("pelt", &format!("invalid option -- '{}'", ch)),
                    }
                }
                continue;
            }
        }
        operands.push(a);
    }

    let on = mode.resolve(stdout_is_console);

    let cwd_holder = if operands.is_empty() {
        Some(env::current_dir().unwrap_or_else(|_| String::from("/")))
    } else {
        None
    };
    if let Some(ref c) = cwd_holder {
        operands.push(c.as_str());
    }

    let mut status = 0;
    let mut out = io::OutSink::new();
    for &path in &operands {
        let md = fs::metadata(path).ok();
        let rkind = match &md {
            Some(m) => meta::kind_of(m),
            None => Kind::Dir, // unstattable operand: treat as a dir to attempt
        };
        // The root line: the path itself, colored by kind.
        let _ = write!(out, "{}{}{}\n", color::col(rkind.color(), on), path, color::reset(on));

        let mut counts = Counts { dirs: 0, files: 0, grafts: 0 };
        // A plain-file operand is just itself; otherwise walk it.
        let is_file = matches!(&md, Some(m) if !m.is_dir());
        if is_file {
            counts.files = 1;
        } else if let Err(e) = walk(&mut out, path, "", 1, max_depth, all, dirs_only, on, &mut counts) {
            eprintln!("pelt: {}: {}", path, e);
            status = 1;
        }

        let _ = write!(
            out,
            "\n{} directory{}, {} file{}",
            counts.dirs,
            if counts.dirs == 1 { "" } else { "ies" },
            counts.files,
            if counts.files == 1 { "" } else { "s" }
        );
        if counts.grafts > 0 {
            let _ = write!(out, ", {} graft{}", counts.grafts, if counts.grafts == 1 { "" } else { "s" });
        }
        out.put(b"\n");
    }
    if out.failed() {
        eprintln!("pelt: write error");
        return 1;
    }
    status
}

/// Read a directory's entries (name + whether readdir called it a directory),
/// dotfile- and dirs-only-filtered, name-sorted.
fn read_entries(dir: &str, all: bool, dirs_only: bool) -> libthyla_rs::err::Result<Vec<(String, bool)>> {
    let mut ents: Vec<(String, bool)> = Vec::new();
    for ent in fs::read_dir(dir)? {
        let e = ent?;
        let rd_dir = e.is_dir();
        let name = e.into_file_name();
        if !all && name.starts_with('.') {
            continue;
        }
        if dirs_only && !rd_dir {
            continue;
        }
        ents.push((name, rd_dir));
    }
    ents.sort_unstable_by(|a, b| a.0.cmp(&b.0));
    Ok(ents)
}

/// Recurse `dir`, emitting one rail-prefixed line per entry. A graft is marked
/// and NOT descended; a real directory is descended until `max_depth`.
#[allow(clippy::too_many_arguments)]
fn walk(
    out: &mut io::OutSink,
    dir: &str,
    prefix: &str,
    depth: usize,
    max_depth: usize,
    all: bool,
    dirs_only: bool,
    on: bool,
    counts: &mut Counts,
) -> libthyla_rs::err::Result<()> {
    let entries = read_entries(dir, all, dirs_only)?;
    let n = entries.len();
    for (i, (name, rd_dir)) in entries.iter().enumerate() {
        let last = i + 1 == n;
        let branch = if last { "└── " } else { "├── " };
        let path = join(dir, name);
        let md = fs::metadata(&path).ok();
        let kind = meta::classify(*rd_dir, &md);
        emit_entry(out, prefix, branch, name, kind, on);
        match kind {
            Kind::Graft => counts.grafts += 1, // boundary: never walk a live namespace
            Kind::Dir => {
                counts.dirs += 1;
                if depth < max_depth {
                    let child = format!("{}{}", prefix, if last { "    " } else { "│   " });
                    // A subdir that is searchable but unreadable (X without R)
                    // is marked inline, not fatal -- the rest of the tree prints.
                    if let Err(e) = walk(out, &path, &child, depth + 1, max_depth, all, dirs_only, on, counts) {
                        let _ = write!(out, "{}{}[unreadable: {}]{}\n", color::col(palette::DIM, on), child, e, color::reset(on));
                    }
                }
            }
            _ => counts.files += 1,
        }
    }
    Ok(())
}

/// One tree line: the rails (dim) + the colored, classified name + a `(graft)`
/// marker for a graft. Color off -> plain rails + name + marker (still useful).
fn emit_entry(out: &mut io::OutSink, prefix: &str, branch: &str, name: &str, kind: Kind, on: bool) {
    let _ = write!(out, "{}{}{}{}", color::col(palette::DIM, on), prefix, branch, color::reset(on));
    let _ = write!(out, "{}{}{}{}", color::col(kind.color(), on), name, kind.suffix(), color::reset(on));
    if kind == Kind::Graft {
        let _ = write!(out, "{}  (graft){}", color::col(palette::DIM, on), color::reset(on));
    }
    out.put(b"\n");
}

/// `--color=auto` resolution -- parked color-on until a kernel TTY check
/// (SYS_FD_DEVCLASS) lands; see ls.rs / the design doc.
fn stdout_is_console() -> bool {
    true
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
