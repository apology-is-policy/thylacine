// rmdir DIR... -- remove empty directories.
//
// Each operand must be an empty directory (the kernel/9P rmdir rejects a
// non-empty dir). Use `rm -r` to remove a tree. Uses libthyla-rs::fs::remove_dir
// (SYS_UNLINK + REMOVEDIR).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::{eprintln, fs};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut had = false;
    let mut status = 0;
    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("rmdir: invalid UTF-8 in operand");
                status = 1;
                continue;
            }
        };
        if let Err(e) = fs::remove_dir(path) {
            eprintln!("rmdir: {}: {}", path, e);
            status = 1;
        }
    }
    if !had {
        eprintln!("rmdir: missing operand");
        return 1;
    }
    status
}
