// yes [STRING...] -- repeatedly print a line until the write fails.
//
// Prints the operands joined by spaces (or "y" if none) followed by a
// newline, forever. Terminates when stdout's write errors -- e.g. the pipe
// reader closed (BrokenPipe), or fd 1 is unwired at v1.0 (G06), in which
// case it exits immediately rather than spinning.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::{Args, Write};

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut line = String::new();
    let mut first = true;
    for op in args.operands() {
        if !first {
            line.push(' ');
        }
        match core::str::from_utf8(op) {
            Ok(s) => line.push_str(s),
            Err(_) => line.push('?'),
        }
        first = false;
    }
    if first {
        line.push('y');
    }
    line.push('\n');

    let mut out = aux_rt::stdout();
    let bytes = line.as_bytes();
    loop {
        if out.write_all(bytes).is_err() {
            break;
        }
    }
    0
}
