// realpath PATH... -- canonicalize a path LEXICALLY.
//
// This is `realpath -m -s` semantics (no symlink resolution, no existence
// requirement) because there is no readlink surface (DOC-GAP G11). It
// collapses `.`, `..`, and `//`. A relative input is anchored against the
// per-Proc cwd (LS-4: env::current_dir / SYS_GETCWD); the join is then
// collapsed lexically. Full symlink-resolving realpath lands with G11.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn normalize(path: &str) -> String {
    let mut stack: Vec<&str> = Vec::new();
    for comp in path.split('/') {
        match comp {
            "" | "." => {}
            ".." => {
                stack.pop();
            }
            c => stack.push(c),
        }
    }
    let mut out = String::from("/");
    out.push_str(&stack.join("/"));
    out
}

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("realpath: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        let abs = if path.starts_with('/') {
            String::from(path)
        } else {
            // Anchor a relative path against the per-Proc cwd (LS-4).
            match env::current_dir() {
                Ok(mut cwd) => {
                    if !cwd.ends_with('/') {
                        cwd.push('/');
                    }
                    cwd.push_str(path);
                    cwd
                }
                Err(e) => {
                    eprintln!("realpath: {}: cannot read cwd: {}", path, e);
                    status = 1;
                    continue;
                }
            }
        };
        let canon = normalize(&abs);
        io::out(canon.as_bytes());
        io::out(b"\n");
    }
    if !had {
        eprintln!("realpath: missing operand");
        return 1;
    }
    status
}
