// rm [-rRfv] FILE... -- remove files (and, with -r, directory trees).
//
// -r / -R: recursively remove a directory and its contents.
// -f:      ignore nonexistent operands; never fail on a missing target.
//
// Each operand and entry is examined without following a link, so a link is
// removed itself, never the directory it names, and needs no -r. An operand
// written with a trailing slash names a directory (POSIX 4.13): a link so
// written is followed, its directory emptied, and the link itself refused as
// not a directory. The walk goes by descriptor (coreutils::rmtree): each
// directory is opened relative to its parent and emptied relative to itself,
// so a link swapped in mid-walk cannot steer it elsewhere. The first failure
// ends an operand and is said once. An operand that names no entry of a
// directory ("/", or one ending in "." or "..") is refused.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::copy::split;
use coreutils::rmtree::{self, Rm};
use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::{File, Metadata};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: rm [-rRfv] FILE...
  Remove files (and, with -r, directory trees).
  -r, -R  remove directories recursively
  -f      ignore nonexistent operands; never prompt
  -v      verbose: print each removed file
  --help  show this help

Examples:
  rm file               # remove a file
  rm -r dir             # remove a directory tree
  rm -v a b c           # report each removal
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut recursive = false;
    let mut force = false;
    let mut verbose = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'r' | 'R' => recursive = true,
                    'f' => force = true,
                    'v' => verbose = true,
                    _ => {
                        eprintln!("rm: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut had = false;
    let mut status = 0;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("rm: invalid UTF-8 in operand");
                status = 1;
                continue;
            }
        };
        if !recursive && lstat(path).is_ok_and(|m| m.is_dir()) {
            eprintln!("rm: cannot remove '{}': Is a directory", path);
            status = 1;
            continue;
        }
        let Some((dir, name)) = split(path) else {
            eprintln!("rm: refusing to remove '{}'", path);
            status = 1;
            continue;
        };
        let r = Rm {
            tool: "rm",
            recursive,
            keep_going: false,
            verbose,
            missing_ok: force,
            only: None,
        };
        let gone = File::open_with_opath(dir).and_then(|parent| {
            rmtree::remove_at(&parent, name, path, path.ends_with('/'), None, &r)
        });
        match gone {
            Ok(_) => {}
            // -f: a missing target is success, not an error.
            Err(Error::NotFound) if force => {}
            Err(e) => {
                eprintln!("rm: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had && !force {
        eprintln!("rm: missing operand");
        return 1;
    }
    status
}

/// What `path` names itself, a link not followed.
fn lstat(path: &str) -> Result<Metadata> {
    File::open_link(path).and_then(|f| f.metadata())
}
