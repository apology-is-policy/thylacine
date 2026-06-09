// cmp FILE1 FILE2 -- byte-compare two files.
//
// Exit 0 if identical; 1 if they differ (prints "FILE1 FILE2 differ: byte N,
// line M" to stdout, GNU-style; or "EOF on SHORTER" to stderr if one is a
// prefix of the other); 2 on an open/read error. Reads both files fully.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::{eprintln, println};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let (p1, p2) = match (args.get_str(1), args.get_str(2)) {
        (Some(a), Some(b)) => (a, b),
        _ => {
            eprintln!("cmp: missing operand");
            return 2;
        }
    };

    let d1 = match File::open(p1).and_then(|mut f| io::slurp(&mut f)) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("cmp: {}: {}", p1, e);
            return 2;
        }
    };
    let d2 = match File::open(p2).and_then(|mut f| io::slurp(&mut f)) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("cmp: {}: {}", p2, e);
            return 2;
        }
    };

    let min = d1.len().min(d2.len());
    let mut line = 1usize;
    for i in 0..min {
        if d1[i] != d2[i] {
            println!("{} {} differ: byte {}, line {}", p1, p2, i + 1, line);
            return 1;
        }
        if d1[i] == b'\n' {
            line += 1;
        }
    }
    if d1.len() != d2.len() {
        let shorter = if d1.len() < d2.len() { p1 } else { p2 };
        eprintln!("cmp: EOF on {}", shorter);
        return 1;
    }
    0
}
