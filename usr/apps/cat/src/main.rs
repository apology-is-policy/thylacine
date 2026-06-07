// cat [FILE...] -- concatenate files (or stdin) to stdout.
//
// With no operands (or "-"), copies stdin. Exercises libthyla-rs::fs::File
// (absolute paths only) + io::Read + the aux-rt streaming copy. No flags
// (-n / -A etc. are deferred).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::fs::File;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut out = aux_rt::stdout();
    let mut had = false;

    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                aux_rt::eprintln!("cat: invalid UTF-8 in path operand");
                status = 1;
                continue;
            }
        };
        if path == "-" {
            if let Err(e) = aux_rt::copy(&mut aux_rt::stdin(), &mut out) {
                aux_rt::eprintln!("cat: -: {}", e);
                status = 1;
            }
            continue;
        }
        match File::open(path) {
            Ok(mut f) => {
                if let Err(e) = aux_rt::copy(&mut f, &mut out) {
                    aux_rt::eprintln!("cat: {}: {}", path, e);
                    status = 1;
                }
            }
            Err(e) => {
                aux_rt::eprintln!("cat: {}: {}", path, e);
                status = 1;
            }
        }
    }

    if !had {
        if let Err(e) = aux_rt::copy(&mut aux_rt::stdin(), &mut out) {
            aux_rt::eprintln!("cat: stdin: {}", e);
            status = 1;
        }
    }
    status
}
