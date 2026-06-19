// realm [--color[=WHEN]] PATH... -- print each path's namespace realm.
//
// The Thylacine answer to "what KIND of thing is this in the namespace?": a real
// filesystem object (fs), a device (dev), or a live kernel-served graft. A graft
// is a path `readdir` lists but `fstat` cannot cross (no stat_native) -- the
// distinction Unix has no word for. Sharpens to the exact Dev class once a kernel
// SYS_FD_DEVCLASS lands (COREUTILS-THYLACINE-DESIGN.md). A presentation tool ->
// color on by default.

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
usage: realm [--color[=WHEN]] PATH...
  Print each PATH's namespace realm: fs (a filesystem object), dev (a
  device), or graft (a live kernel-served namespace mount, which fstat
  cannot cross).
  --color[=WHEN]  colorize: always (default) | never | auto
  --help          show this help

Examples:
  realm /               # fs
  realm /srv            # graft (a live kernel namespace)
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

/// `(realm, color)` for `path`. Empty realm means "no such path" (the parent
/// does not list it either).
fn realm_of(path: &str) -> (&'static str, &'static str) {
    match fs::metadata(path) {
        Ok(m) => {
            let k = meta::kind_of(&m);
            (k.realm(), k.color())
        }
        // fstat failed: a graft (the parent lists it -- a mount point with no
        // stat_native) vs a path that does not exist.
        Err(_) => {
            if parent_lists(path) {
                ("graft", palette::VIOLET)
            } else {
                ("", palette::FG)
            }
        }
    }
}

/// Whether `path`'s parent directory lists its basename (so the path EXISTS even
/// though fstat could not cross into it -- the graft signature).
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
    let mut mode = ColorMode::Always; // presentation tool
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
                    None => return usage::die("realm", &format!("invalid --color value -- '{}'", w)),
                }
                continue;
            }
            if a.starts_with('-') && a != "-" && a.len() > 1 {
                return usage::die("realm", &format!("invalid option -- '{}'", a));
            }
        }
        paths.push(a);
    }
    if paths.is_empty() {
        return usage::die("realm", "missing operand");
    }

    let on = mode.resolve(stdout_is_console);
    let mut out = io::OutSink::new();
    let mut status = 0;
    for path in &paths {
        let (realm, color) = realm_of(path);
        if realm.is_empty() {
            eprintln!("realm: {}: no such path", path);
            status = 1;
            continue;
        }
        let _ = write!(
            out,
            "{}{:<5}{}  {}{}{}\n",
            color::col(color, on),
            realm,
            color::reset(on),
            color::col(color, on),
            path,
            color::reset(on)
        );
    }
    if out.failed() {
        eprintln!("realm: write error");
        return 1;
    }
    status
}

/// `--color=auto` stub (no cooked-mode TTY check yet; see the SYS_FD_DEVCLASS
/// spec). True so an explicit `--color=auto` colorizes.
fn stdout_is_console() -> bool {
    true
}
