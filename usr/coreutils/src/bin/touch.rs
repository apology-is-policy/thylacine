// touch FILE... -- create FILE if absent (no truncate).
//
// v1.0: creates an empty file when absent, opens-without-truncate when present.
// It does NOT update mtime -- the dev9p FS reports mtime 0 at v1.0 and there is
// no timestamp setter yet (LS-9 / G12). So on an existing file `touch` is an
// access no-op, not a timestamp bump. Uses OpenOptions{write,create} ->
// SYS_WALK_CREATE-or-open (the Stratum FS; devramfs is read-only).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::fs::OpenOptions;

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
                eprintln!("touch: invalid UTF-8 in operand");
                status = 1;
                continue;
            }
        };
        // write + create, NO truncate: create-if-absent, else open the existing
        // file without clearing it (touch semantics).
        match OpenOptions::new().write(true).create(true).open(path) {
            Ok(_f) => { /* dropped -> handle closed */ }
            Err(e) => {
                eprintln!("touch: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        eprintln!("touch: missing operand");
        return 1;
    }
    status
}
