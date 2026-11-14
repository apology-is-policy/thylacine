// cp [-aLPpRrvnf] SRC... DST -- copy files (and, with -r, directory trees).
//
// Forms: `cp SRC DST` (DST a file or existing dir) and `cp SRC... DIR`.
// -r / -R: recursively copy a directory. -p keeps the source's times, owner and
// mode; -a is -R -p -P. -P would copy a symlink as a symlink, which needs a
// creation call native code does not have yet, so a symlink source under -P is
// refused. The copy itself is coreutils::copy, which mv shares.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::copy::{self, base, canon, join, Opts};
use coreutils::usage;
use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::fs;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: cp [-aLPpRrvnf] SRC... DST
  Copy files (and, with -r, directory trees).
  -r, -R  copy directories recursively
  -p      keep each source's times, owner and mode
  -a      archive: -R -p -P
  -P, -d  copy a symbolic link as a link (not supported yet: refused)
  -L      follow symbolic links in the source (the default)
  -v      verbose: print each copy
  -n      no-clobber: never overwrite an existing file
  -f      force (accepted; cp always overwrites and never prompts)
  --help  show this help

Examples:
  cp a b                # copy a file
  cp -r dir copy        # copy a directory tree
  cp -p a b             # copy a file, keeping its times
  cp -v *.txt dest/     # report each copy
";

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut recursive = false;
    let mut verbose = false;
    let mut no_clobber = false;
    let mut preserve = false;
    let mut no_deref = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'r' | 'R' => recursive = true,
                    'v' => verbose = true,
                    'n' => no_clobber = true,
                    'p' => preserve = true,
                    'a' => {
                        recursive = true;
                        preserve = true;
                        no_deref = true;
                    }
                    'P' | 'd' => no_deref = true,
                    'L' => no_deref = false,
                    'f' => {} // force: a no-op (cp always overwrites, never prompts)
                    _ => return usage::die("cp", &format!("invalid option -- '{}'", ch)),
                }
            }
            idx += 1;
        } else {
            break;
        }
    }
    let opts = Opts {
        tool: "cp",
        verbose,
        no_clobber,
        preserve,
        no_deref,
        attrs_soft: false,
        refuse_special: false,
    };

    let mut ops: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => ops.push(p),
            Err(_) => {
                eprintln!("cp: invalid UTF-8 in operand");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        eprintln!("cp: missing file operand");
        return 1;
    }

    let dest = ops[ops.len() - 1];
    let sources = &ops[..ops.len() - 1];
    let dest_is_dir = fs::is_dir(dest);
    if sources.len() > 1 && !dest_is_dir {
        eprintln!("cp: target '{}' is not a directory", dest);
        return 1;
    }

    let mut status = 0;
    for src in sources {
        if fs::is_dir(src) && !recursive {
            eprintln!("cp: -r not specified; omitting directory '{}'", src);
            status = 1;
            continue;
        }
        let target = if dest_is_dir {
            join(dest, base(src))
        } else {
            String::from(dest)
        };
        // Refuse the plain into-itself copy (`cp -r d d/sub`) before anything
        // is created. Lexical, so a symlink can hide it: the walk's dest_root
        // guard catches that case, after the fact.
        if recursive && fs::is_dir(src) {
            if let (Some(mut prefix), Some(ct)) = (canon(src), canon(target.as_str())) {
                if !prefix.ends_with('/') {
                    prefix.push('/');
                }
                if ct.starts_with(prefix.as_str()) {
                    eprintln!(
                        "cp: cannot copy a directory, '{}', into itself, '{}'",
                        src, target
                    );
                    status = 1;
                    continue;
                }
            }
        }
        if !copy::copy(src, target.as_str(), &opts) {
            status = 1;
        }
    }
    status
}
