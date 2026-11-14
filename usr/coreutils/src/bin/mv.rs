// mv [-vnf] SRC... DST -- rename/move files.
//
// Two forms: `mv SRC DST` (rename) and `mv SRC... DIR` (move each into DIR).
// Within one device a move is libthyla-rs::fs::rename (SYS_RENAME / 9P
// Trenameat -- an atomic replace). Across devices the kernel answers EXDEV, and
// mv does what POSIX mv does: the destination removed, the source's tree
// duplicated with its times, owner and mode (coreutils::copy), then the source
// removed. A duplication that fails removes what it made and leaves the source
// whole, so one of the two is always complete.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::copy::{self, base, join, Opts};
use coreutils::usage;
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, File};
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: mv [-vnf] SRC... DST
  Rename or move files. Within one device a move is an atomic rename;
  across devices mv copies (times, owner and mode kept), then removes.
  -v      verbose: print each move
  -n      no-clobber: never overwrite an existing file
  -f      force (accepted; mv never prompts)
  --help  show this help

Examples:
  mv old new            # rename
  mv -v file dir/       # move into a directory
  mv -n a b             # never overwrite b
";

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut verbose = false;
    let mut no_clobber = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'v' => verbose = true,
                    'n' => no_clobber = true,
                    'f' => {} // force: a no-op (mv replaces atomically, never prompts)
                    _ => return usage::die("mv", &format!("invalid option -- '{}'", ch)),
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut ops: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => ops.push(p),
            Err(_) => {
                eprintln!("mv: invalid UTF-8 in operand");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        eprintln!("mv: missing destination operand");
        return 1;
    }

    let dest = ops[ops.len() - 1];
    let sources = &ops[..ops.len() - 1];
    let dest_is_dir = fs::is_dir(dest);
    if sources.len() > 1 && !dest_is_dir {
        eprintln!("mv: target '{}' is not a directory", dest);
        return 1;
    }

    let mut status = 0;
    for src in sources {
        let target = if dest_is_dir {
            join(dest, base(src))
        } else {
            String::from(dest)
        };
        if no_clobber && fs::metadata(target.as_str()).is_ok() {
            continue; // -n: never overwrite an existing destination
        }
        match fs::rename(src, target.as_str()) {
            Ok(()) => {
                if verbose {
                    println!("'{}' -> '{}'", src, target);
                }
            }
            Err(Error::CrossesDevices) => {
                if !move_across(src, target.as_str(), verbose) {
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("mv: {} -> {}: {}", src, target, e);
                status = 1;
            }
        }
    }
    status
}

/// The kind of what `path` names itself, a link not followed.
fn lstat(path: &str) -> Result<fs::Metadata, Error> {
    File::open_link(path).and_then(|f| f.metadata())
}

fn move_across(src: &str, dst: &str, verbose: bool) -> bool {
    let sm = match lstat(src) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("mv: cannot stat '{}': {}", src, e);
            return false;
        }
    };
    if sm.is_symlink() {
        eprintln!("mv: cannot move symbolic link '{}' to another device: creating a link is not supported", src);
        return false;
    }
    // What a rename would have replaced: a file by a file, an empty directory by
    // a directory.
    if let Ok(dm) = lstat(dst) {
        if sm.is_dir() && !dm.is_dir() {
            eprintln!(
                "mv: cannot overwrite non-directory '{}' with directory '{}'",
                dst, src
            );
            return false;
        }
        if !sm.is_dir() && dm.is_dir() {
            eprintln!(
                "mv: cannot overwrite directory '{}' with non-directory '{}'",
                dst, src
            );
            return false;
        }
        let gone = if dm.is_dir() {
            fs::remove_dir(dst)
        } else {
            fs::remove_file(dst)
        };
        if let Err(e) = gone {
            eprintln!("mv: cannot remove '{}': {}", dst, e);
            return false;
        }
    }
    let o = Opts {
        tool: "mv",
        verbose: false,
        no_clobber: false,
        preserve: true,
        no_deref: true,
        attrs_soft: true,
    };
    if !copy::copy(src, dst, &o) {
        if !remove_tree(dst) {
            eprintln!("mv: '{}' is left partly copied; '{}' is whole", dst, src);
        }
        return false;
    }
    if !remove_tree(src) {
        return false;
    }
    if verbose {
        println!("'{}' -> '{}'", src, dst);
    }
    true
}

/// `rm -r` of `path`, links not followed: a diagnostic for each failure; true
/// when nothing is left. A path already gone is nothing left.
fn remove_tree(path: &str) -> bool {
    let m = match lstat(path) {
        Ok(m) => m,
        Err(Error::NotFound) => return true,
        Err(e) => {
            eprintln!("mv: cannot stat '{}': {}", path, e);
            return false;
        }
    };
    if !m.is_dir() {
        return match fs::remove_file(path) {
            Ok(()) => true,
            Err(e) => {
                eprintln!("mv: cannot remove '{}': {}", path, e);
                false
            }
        };
    }
    let mut ok = true;
    match fs::read_dir(path) {
        Ok(entries) => {
            for ent in entries {
                match ent {
                    Ok(e) => {
                        let name = e.file_name();
                        if name != "." && name != ".." && !remove_tree(&join(path, name)) {
                            ok = false;
                        }
                    }
                    Err(e) => {
                        eprintln!("mv: reading directory '{}': {}", path, e);
                        ok = false;
                        break;
                    }
                }
            }
        }
        Err(e) => {
            eprintln!("mv: cannot read directory '{}': {}", path, e);
            ok = false;
        }
    }
    if ok {
        if let Err(e) = fs::remove_dir(path) {
            eprintln!("mv: cannot remove '{}': {}", path, e);
            ok = false;
        }
    }
    ok
}
