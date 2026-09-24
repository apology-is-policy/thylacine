// cmp FILE1 FILE2 -- byte-compare two files.
//
// Exit 0 if identical; 1 if they differ (prints "FILE1 FILE2 differ: byte N,
// line M" to stdout, GNU-style; or "EOF on SHORTER" to stderr if one is a
// prefix of the other); 2 on an open/read error. Streams both files through
// two fixed buffers (coreutils::stream), so no file is too large to compare.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use coreutils::stream::{self, Diff};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io::Read;
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: cmp FILE1 FILE2
  Compare two files byte by byte. Silent + exit 0 if identical; prints
  'differ: byte N, line M' + exit 1 if they differ; exit 2 on error.
  --help  show this help

Examples:
  cmp a.bin b.bin     # nothing printed if identical
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let (p1, p2) = match (args.get_str(1), args.get_str(2)) {
        (Some(a), Some(b)) => (a, b),
        _ => {
            eprintln!("cmp: missing operand");
            return 2;
        }
    };

    let mut f1 = match File::open(p1) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("cmp: {}: {}", p1, e);
            return 2;
        }
    };
    let mut f2 = match File::open(p2) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("cmp: {}: {}", p2, e);
            return 2;
        }
    };

    let name = |side| if side == 0 { p1 } else { p2 };
    match stream::compare(|b| f1.read(b), |b| f2.read(b)) {
        Ok(Diff::Same) => 0,
        Ok(Diff::At { byte, line }) => {
            println!("{} {} differ: byte {}, line {}", p1, p2, byte, line);
            1
        }
        Ok(Diff::Eof(side)) => {
            eprintln!("cmp: EOF on {}", name(side));
            1
        }
        Err((side, e)) => {
            eprintln!("cmp: {}: {}", name(side), e);
            2
        }
    }
}
