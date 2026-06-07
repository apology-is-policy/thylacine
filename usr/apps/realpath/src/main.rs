// realpath PATH... -- canonicalize a path LEXICALLY.
//
// PARTIAL at v1.0: this is `realpath -m -s` semantics (no symlink
// resolution, no existence requirement) because there is no readlink
// surface (DOC-GAP G11) and no cwd to resolve a relative path against
// (G07). It collapses `.`, `..`, and `//` lexically. A relative input is
// rejected (no cwd to anchor it).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;

aux_rt::main!(run);

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
                aux_rt::eprintln!("realpath: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if !path.starts_with('/') {
            // No cwd at v1.0 (G07): cannot anchor a relative path.
            aux_rt::eprintln!("realpath: {}: relative paths unsupported (no cwd)", path);
            status = 1;
            continue;
        }
        let canon = normalize(path);
        aux_rt::out(canon.as_bytes());
        aux_rt::out(b"\n");
    }
    if !had {
        aux_rt::eprintln!("realpath: missing operand");
        return 1;
    }
    status
}
