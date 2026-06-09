// which NAME... -- locate a command.
//
// DEGENERATE at v1.0 (DOC-GAP G15): there is no PATH environment variable
// (no envp surface), so a bare command name cannot be resolved (reported
// not-found, exit 1, like `which` on a miss). A NAME containing '/' is
// treated as an explicit path (absolute, or relative-to-cwd since LS-4) and
// probed for existence.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::io;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let name = match core::str::from_utf8(op) {
            Ok(n) => n,
            Err(_) => {
                status = 1;
                continue;
            }
        };
        if name.contains('/') {
            // Explicit path: report it if it exists (fs::exists resolves a
            // relative path against the cwd since LS-4).
            if libthyla_rs::fs::exists(name) {
                io::out(name.as_bytes());
                io::out(b"\n");
            } else {
                status = 1;
            }
        } else {
            // No PATH to search (G15): cannot resolve a bare command name.
            status = 1;
        }
    }
    if !had {
        return 1;
    }
    status
}
