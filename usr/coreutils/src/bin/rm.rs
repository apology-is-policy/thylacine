// rm [-rRf] FILE... -- remove files (and, with -r, directory trees).
//
// -r / -R: recursively remove a directory and its contents.
// -f:      ignore nonexistent operands; never fail on a missing target.
//
// Recursion skips "." and ".." (fs::read_dir yields them -- Stratum returns
// cookie 1 = ".", cookie 2 = ".."), so the walk cannot loop or escape upward.
// Recursion depth tracks FS depth; deep trees are an interactive, non-adversarial
// case at v1.0 (an explicit-stack rewrite is a v1.x refinement).

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
        if !recursive && fs::is_dir(path) {
            eprintln!("rm: cannot remove '{}': Is a directory", path);
            status = 1;
            continue;
        }
        if let Err(e) = remove(path, recursive, force, verbose) {
            eprintln!("rm: {}: {}", path, e);
            status = 1;
        }
    }
    if !had && !force {
        eprintln!("rm: missing operand");
        return 1;
    }
    status
}

fn remove(path: &str, recursive: bool, force: bool, verbose: bool) -> Result<()> {
    match fs::metadata(path) {
        Ok(m) if m.is_dir() => {
            if !recursive {
                return Err(Error::InvalidArgument); // "is a directory" (need -r)
            }
            for ent in fs::read_dir(path)? {
                let e = ent?;
                let name = e.file_name();
                if name == "." || name == ".." {
                    continue;
                }
                remove(&join(path, name), recursive, force, verbose)?;
            }
            fs::remove_dir(path)?;
            if verbose {
                println!("removed directory '{}'", path);
            }
            Ok(())
        }
        Ok(_) => {
            fs::remove_file(path)?;
            if verbose {
                println!("removed '{}'", path);
            }
            Ok(())
        }
        // -f: a missing target is success, not an error.
        Err(Error::NotFound) if force => Ok(()),
        Err(e) => Err(e),
    }
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
