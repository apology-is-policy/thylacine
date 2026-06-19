// yes [STRING...] -- repeatedly print a line until the write fails.
//
// Prints the operands joined by spaces (or "y" if none) followed by a
// newline, forever. Terminates when stdout's write errors -- e.g. the pipe
// reader closed (BrokenPipe), or fd 1 is unwired, in which case it exits
// immediately rather than spinning.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::io::{self, Write};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: yes [STRING...]
  Print STRING (or 'y' if none) on its own line, repeatedly, until the write
  fails (e.g. the pipe reader closes).
  --help  show this help

Examples:
  yes | head -3         # y / y / y
  yes ok | head -1      # ok
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

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

    let mut out = io::stdout();
    let bytes = line.as_bytes();
    loop {
        if out.write_all(bytes).is_err() {
            break;
        }
    }
    0
}
