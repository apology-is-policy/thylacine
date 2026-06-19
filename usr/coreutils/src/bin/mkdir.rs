// mkdir [-p] DIR... -- create directories.
//
// -p: create missing parent directories as needed, and do not error if a
// directory already exists. Uses libthyla-rs::fs::create_dir (SYS_WALK_CREATE
// + DMDIR). Absolute paths only at v1.0 (no cwd until LS-4).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::{Error, Result};
use libthyla_rs::{eprintln, fs, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: mkdir [-pv] DIR...
  Create directories.
  -p      create missing parents; no error if a directory exists
  -v      verbose: print each created directory
  --help  show this help

Examples:
  mkdir newdir          # one directory
  mkdir -p a/b/c        # create missing parents
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut parents = false;
    let mut verbose = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'p' => parents = true,
                    'v' => verbose = true,
                    _ => {
                        eprintln!("mkdir: invalid option -- '{}'", ch);
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
                eprintln!("mkdir: invalid UTF-8 in operand");
                status = 1;
                continue;
            }
        };
        let r = if parents {
            mkdir_p(path, verbose)
        } else {
            match fs::create_dir(path) {
                Ok(()) => {
                    if verbose {
                        println!("mkdir: created directory '{}'", path);
                    }
                    Ok(())
                }
                Err(e) => Err(e),
            }
        };
        if let Err(e) = r {
            eprintln!("mkdir: {}: {}", path, e);
            status = 1;
        }
    }
    if !had {
        eprintln!("mkdir: missing operand");
        return 1;
    }
    status
}

// `mkdir -p`: create each ancestor then the leaf, treating an already-present
// component as success. A relative path is resolved against the per-Proc cwd
// (LS-4) so the ancestor loop walks the real absolute chain; pre-LS-4 this
// short-circuited a relative path into a single create_dir, so `mkdir -p a/b`
// silently failed for the form every interactive user types (RW-9 R4-F4).
fn mkdir_p(path: &str, verbose: bool) -> Result<()> {
    let joined;
    let abs = if path.starts_with('/') {
        path
    } else {
        let cwd = env::current_dir().unwrap_or_else(|_| String::from("/"));
        let mut s = String::from(cwd.trim_end_matches('/'));
        s.push('/');
        s.push_str(path);
        joined = s;
        joined.as_str()
    };
    let mut acc = String::new();
    for comp in abs.split('/').filter(|s| !s.is_empty()) {
        acc.push('/');
        acc.push_str(comp);
        match fs::create_dir(acc.as_str()) {
            // -v reports only the components actually created (not the
            // ones that already existed -- matches GNU mkdir -pv).
            Ok(()) => {
                if verbose {
                    println!("mkdir: created directory '{}'", acc);
                }
            }
            Err(Error::Exists) => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}
