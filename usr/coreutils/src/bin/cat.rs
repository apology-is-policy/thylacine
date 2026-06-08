// cat [FILE...] -- concatenate files (or stdin) to stdout.
//
// With no operands (or "-"), copies stdin. Uses libthyla-rs::fs::File
// (absolute paths only) + io::Read + the streaming io::copy. No flags
// (-n / -A etc. are deferred).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut out = io::stdout();
    let mut had = false;

    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("cat: invalid UTF-8 in path operand");
                status = 1;
                continue;
            }
        };
        if path == "-" {
            if let Err(e) = io::copy(&mut io::stdin(), &mut out) {
                eprintln!("cat: -: {}", e);
                status = 1;
            }
            continue;
        }
        match File::open(path) {
            Ok(mut f) => {
                if let Err(e) = io::copy(&mut f, &mut out) {
                    eprintln!("cat: {}: {}", path, e);
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("cat: {}: {}", path, e);
                status = 1;
            }
        }
    }

    if !had {
        if let Err(e) = io::copy(&mut io::stdin(), &mut out) {
            eprintln!("cat: stdin: {}", e);
            status = 1;
        }
    }
    status
}
