// mv [-vnf] SRC... DST -- rename/move files.
//
// Two forms: `mv SRC DST` (rename) and `mv SRC... DIR` (move each into DIR).
// Uses libthyla-rs::fs::rename (SYS_RENAME / 9P Trenameat -- atomic replace).
// v1.0: same-Dev only (the whole pivoted FS is one Stratum Dev, so any in-tree
// move qualifies); a cross-Dev move (copy+remove fallback) is a v1.x refinement.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::usage;
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{self, Path};
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: mv [-vnf] SRC... DST
  Rename or move files (atomic replace, same-Dev at v1.0).
  -v      verbose: print each move
  -n      no-clobber: never overwrite an existing file
  -f      force (accepted; mv replaces atomically and never prompts)
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
        if let Err(e) = fs::rename(src, target.as_str()) {
            eprintln!("mv: {} -> {}: {}", src, target, e);
            status = 1;
        } else if verbose {
            println!("'{}' -> '{}'", src, target);
        }
    }
    status
}

fn base(path: &str) -> &str {
    Path::new(path).file_name().unwrap_or(path)
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
