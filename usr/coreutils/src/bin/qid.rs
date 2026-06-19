// qid [--color[=WHEN]] PATH... -- print each path's 9P qid.
//
// The Plan-9 identity the kernel knows an object by: type:version:path. Unix
// hides the inode behind `ls -i`; Thylacine foregrounds the qid. A graft has no
// qid (fstat cannot cross it). A presentation tool -> color on by default.

#![no_std]
#![no_main]
#![allow(clippy::write_with_newline)] // a trailing \n in a color-formatted line reads naturally

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use core::fmt::Write as _;
use coreutils::color::{self, ColorMode};
use coreutils::{meta, palette, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs;
use libthyla_rs::{eprintln, io};

const USAGE: &str = "\
usage: qid [--color[=WHEN]] PATH...
  Print each PATH's 9P qid (type:version:path) -- the Plan-9 identity the
  kernel knows the object by (type d/f/c). A graft has no qid: fstat
  cannot cross a live kernel namespace.
  --color[=WHEN]  colorize: always (default) | never | auto
  --help          show this help

Examples:
  qid /                 # the root's 9P qid
  qid file
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

/// Whether `path`'s parent lists its basename (it exists despite fstat failing
/// -- the graft signature, distinct from a missing path).
fn parent_lists(path: &str) -> bool {
    let trimmed = path.trim_end_matches('/');
    let (parent, base) = match trimmed.rfind('/') {
        Some(0) => ("/", &trimmed[1..]),
        Some(i) => (&trimmed[..i], &trimmed[i + 1..]),
        None => (".", trimmed),
    };
    if base.is_empty() {
        return false;
    }
    if let Ok(rd) = fs::read_dir(parent) {
        for ent in rd.flatten() {
            if ent.file_name() == base {
                return true;
            }
        }
    }
    false
}

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut mode = ColorMode::Always;
    let mut paths: Vec<&str> = Vec::new();
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
                    None => return usage::die("qid", &format!("invalid --color value -- '{}'", w)),
                }
                continue;
            }
            if a.starts_with('-') && a != "-" && a.len() > 1 {
                return usage::die("qid", &format!("invalid option -- '{}'", a));
            }
        }
        paths.push(a);
    }
    if paths.is_empty() {
        return usage::die("qid", "missing operand");
    }

    let on = mode.resolve(stdout_is_console);
    let mut out = io::OutSink::new();
    let mut status = 0;
    for path in &paths {
        match fs::metadata(path) {
            Ok(m) => {
                let color = meta::kind_of(&m).color();
                let _ = write!(
                    out,
                    "{}{:<14}{}  {}{}{}\n",
                    color::col(palette::GOLD, on),
                    meta::qid_full(&m),
                    color::reset(on),
                    color::col(color, on),
                    path,
                    color::reset(on)
                );
            }
            Err(_) => {
                if parent_lists(path) {
                    // A graft: it exists, but fstat cannot cross it -> no qid.
                    let _ = write!(
                        out,
                        "{}{:<14}{}  {}{}{}\n",
                        color::col(palette::VIOLET, on),
                        "graft",
                        color::reset(on),
                        color::col(palette::VIOLET, on),
                        path,
                        color::reset(on)
                    );
                } else {
                    eprintln!("qid: {}: no such path", path);
                    status = 1;
                }
            }
        }
    }
    if out.failed() {
        eprintln!("qid: write error");
        return 1;
    }
    status
}

/// `--color=auto` stub (no cooked-mode TTY check yet; see the SYS_FD_DEVCLASS
/// spec). True so an explicit `--color=auto` colorizes.
fn stdout_is_console() -> bool {
    true
}
