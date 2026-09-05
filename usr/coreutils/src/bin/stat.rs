// stat [--color[=WHEN]] [--beacon=WHEN] FILE... -- display file metadata (via
// SYS_FSTAT), the Thylacine way: the familiar GNU-shaped block, colored, with
// the Realm + the full 9P Qid foregrounded (COREUTILS-THYLACINE-DESIGN.md). A
// presentation tool -> color on the console (auto); `--color=never` for a plain block.
//
// Beacon (docs/BEACON.md): at the Rich tier the SAME plain block goes out with
// an `obj type=path` frame on the subject (the File: value; cleaned absolute
// ref) and SGR off -- strip(rich) == the plain block byte-exactly. (12.7's
// `table` op for stat is deferred: the GNU block is deliberately NOT tabular;
// a table would break the strip identity. Recorded in BEACON.md 12.5.)
//
// atime/mtime/ctime are 0 at v1.0 (most Devs don't track timestamps).

#![no_std]
#![no_main]
#![allow(clippy::write_with_newline)] // a trailing \n in a color-formatted line reads naturally

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::fmt::Write as _;
use coreutils::color::{self, ColorMode};
use coreutils::{meta, palette, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::fs::{self, Metadata};
use libthyla_rs::io;

const USAGE: &str = "\
usage: stat [--color[=WHEN]] [--beacon=WHEN] FILE...
  Show file metadata: perms, owner, size, the namespace REALM, and the full
  9P QID (type:version:path). A graft has no introspectable metadata.
  --color[=WHEN]  colorize: always | never | auto (default)
  --beacon=WHEN   semantic markup: auto (default) | always | never
  --help  show this help

Examples:
  stat file             # perms, owner, size, realm, qid
  stat /home
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn type_word(m: &Metadata) -> &'static str {
    if m.is_dir() {
        "directory"
    } else if m.is_char_device() {
        "character special file"
    } else if m.is_symlink() {
        "symbolic link"
    } else {
        "regular file"
    }
}

fn print_one(out: &mut io::OutSink, path: &str, m: &Metadata, on: bool, rich: bool) {
    let dim = color::col(palette::DIM, on);
    let rst = color::reset(on);
    // Kind / realm / perms / owner / qid presentation are shared via meta.
    let kind = meta::kind_of(m);
    let kc = kind.color();
    let ps = meta::perms_string(m);

    if rich {
        // The subject presentation: same plain bytes, obj-framed (no SGR --
        // `on` is false whenever `rich` holds; the gate in run() forces it).
        out.put(b"  File: ");
        {
            let mut sout = coreutils::beacon_gate::SinkOut(out);
            let mut s = beacon::sink::Sink::new(&mut sout, beacon::Tier::Rich);
            match coreutils::path::abs(path) {
                Some(r) => s.obj(beacon::sink::ObjType::Path, &r, path),
                None => s.text(path),
            }
        }
        out.put(b"\n");
    } else {
        let _ = write!(out, "{}  File:{} {}{}{}\n", dim, rst, color::col(kc, on), path, rst);
    }
    let _ = write!(
        out,
        "{}  Size:{} {}   {}Blocks:{} {}   {}IO Block:{} {}   {}{}{}\n",
        dim, rst, m.len(),
        dim, rst, m.blocks(),
        dim, rst, m.blksize(),
        color::col(kc, on), type_word(m), rst
    );
    let _ = write!(
        out,
        "{} Realm:{} {}{}{}   {}Qid:{} {}{}{}   {}Links:{} {}\n",
        dim, rst, color::col(kc, on), kind.realm(), rst,
        dim, rst, color::col(palette::GOLD, on), meta::qid_full(m), rst,
        dim, rst, m.nlink()
    );
    let _ = write!(
        out,
        "{}  Mode:{} ({:04o}/{}{}{})   {}Uid:{} {}   {}Gid:{} {}\n",
        dim, rst,
        m.permissions() & 0o7777,
        color::col(kc, on), ps, rst,
        dim, rst, meta::owner(m.uid()),
        dim, rst, meta::owner(m.gid())
    );
    let _ = write!(
        out,
        "{}Access: {}   Modify: {}   Change: {}{}\n",
        dim, m.atime_sec(), m.mtime_sec(), m.ctime_sec(), rst
    );
}

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut mode = ColorMode::Auto; // color iff console (the H-1 SYS_FD_DEVCLASS unification)
    let mut bmode = beacon::BeaconMode::Auto;
    let mut status = 0;
    let mut out = io::OutSink::new();
    let mut had = false;
    let mut opts_done = false;

    // Collect operands, honoring --color / --beacon and --.
    let mut paths: alloc::vec::Vec<&str> = alloc::vec::Vec::new();
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
            if let Some(when) = a.strip_prefix("--color=") {
                match ColorMode::parse_when(when) {
                    Some(m) => mode = m,
                    None => {
                        eprintln!("stat: invalid --color value -- '{}'", when);
                        usage::hint("stat");
                        return 2;
                    }
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
                    None => {
                        eprintln!("stat: invalid --beacon value -- '{}'", when);
                        usage::hint("stat");
                        return 2;
                    }
                }
                continue;
            }
        }
        paths.push(a);
    }

    // The emission gate (BEACON.md 12.4); SGR is off inside rich output.
    let rich = coreutils::beacon_gate::resolve(bmode) == beacon::Tier::Rich;
    let on = !rich && mode.resolve(stdout_is_console);

    for path in &paths {
        had = true;
        match fs::metadata(path) {
            Ok(m) => print_one(&mut out, path, &m, on, rich),
            Err(e) => {
                // An unstattable path is very likely a graft (a live kernel
                // namespace with no stat_native) -- name that, don't just errno.
                eprintln!("stat: {}: {} (a graft has no introspectable metadata; try ls)", path, e);
                status = 1;
            }
        }
    }
    if !had {
        return usage::die("stat", "missing operand");
    }
    if out.failed() {
        eprintln!("stat: write error");
        return 1;
    }
    status
}

/// `--color=auto`: stdout is the interactive console iff its Dev class is
/// `'c'` (`SYS_FD_DEVCLASS`; H-1 closed the long-parked `true` stub).
fn stdout_is_console() -> bool {
    libthyla_rs::stdout_is_terminal()
}
