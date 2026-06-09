// mv SRC... DST -- rename/move files.
//
// Two forms: `mv SRC DST` (rename) and `mv SRC... DIR` (move each into DIR).
// Uses libthyla-rs::fs::rename (SYS_RENAME / 9P Trenameat -- atomic replace).
// v1.0: same-Dev only (the whole pivoted FS is one Stratum Dev, so any in-tree
// move qualifies); a cross-Dev move (copy+remove fallback) is a v1.x refinement.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{self, Path};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        // mv has no v1.0 flags; a lone "-" is a literal operand.
        if a.starts_with('-') && a.len() > 1 {
            eprintln!("mv: unknown option '{}'", a);
            return 1;
        }
        break;
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
        if let Err(e) = fs::rename(src, target.as_str()) {
            eprintln!("mv: {} -> {}: {}", src, target, e);
            status = 1;
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
