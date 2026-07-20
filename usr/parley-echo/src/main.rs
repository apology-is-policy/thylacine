// parley-echo -- the echo child for the parley-probe transport E2E (8e-1c).
//
// A minimal, buffering-free stdin -> stdout streaming copy: each read is written
// straight to fd 1 (libthyla-rs Stdout is unbuffered -- every write is an
// immediate SYS_WRITE), so a framed message the parent sends is echoed back
// promptly and the parent's poll wakes. Runs until stdin EOF (the parent closes
// its write end for a graceful shutdown). This is the dedicated-child probe
// pattern (cf. /stack-child, /debug-child): baked into the boot cpio so
// parley-probe can spawn it by root-anchored name, independent of coreutils
// placement OR any coreutil's internal buffering.
//
// Silent by design apart from the echo (no diagnostic I/O -- a concurrent print
// would byte-race the parent's framed stream on the shared console).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::io::{stdin, stdout, Read, Write};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut inp = stdin();
    let mut out = stdout();
    let mut buf = [0u8; 4096];
    loop {
        match inp.read(&mut buf) {
            Ok(0) => return 0, // stdin EOF -- the parent closed its write end
            Ok(n) => {
                if out.write_all(&buf[..n]).is_err() {
                    return 1; // the parent closed its read end
                }
            }
            Err(_) => return 1,
        }
    }
}
