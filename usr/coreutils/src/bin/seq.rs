// seq [FIRST [INCR]] LAST -- print an integer sequence, one per line.
//
// Integer-only at v1 (no floating sequences). FIRST defaults to 1, INCR to
// 1. A zero increment is rejected. Counts up (INCR > 0) or down (INCR < 0).
// Iteration is overflow-safe (stops at the i64 boundary instead of panicking).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;
use alloc::string::String;
use core::fmt::Write as FmtWrite;

use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;
use libthyla_rs::io::{self, Write};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: seq [FIRST [INCR]] LAST
  Print integers from FIRST (default 1) to LAST, stepping by INCR
  (default 1), one per line. INCR may be negative to count down.
  --help  show this help

Examples:
  seq 5               # 1 2 3 4 5 (one per line)
  seq 2 2 10          # 2 4 6 8 10
  seq 10 -2 0         # 10 8 6 4 2 0
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut nops = 0;
    while args.get(1 + nops).is_some() {
        nops += 1;
    }
    let p = |i: usize| args.get_str(i).and_then(|s| s.parse::<i64>().ok());

    let triple = match nops {
        1 => p(1).map(|l| (1i64, 1i64, l)),
        2 => match (p(1), p(2)) {
            (Some(f), Some(l)) => Some((f, 1, l)),
            _ => None,
        },
        3 => match (p(1), p(2), p(3)) {
            (Some(f), Some(i), Some(l)) => Some((f, i, l)),
            _ => None,
        },
        _ => {
            eprintln!("seq: usage: seq [FIRST [INCR]] LAST");
            return 1;
        }
    };
    let (first, incr, last) = match triple {
        Some(t) => t,
        None => {
            eprintln!("seq: invalid number");
            return 1;
        }
    };
    if incr == 0 {
        eprintln!("seq: increment must not be zero");
        return 1;
    }

    let mut i = first;
    let mut out = io::stdout();
    let mut buf = String::new();
    loop {
        if (incr > 0 && i > last) || (incr < 0 && i < last) {
            break;
        }
        buf.clear();
        let _ = write!(&mut buf, "{}\n", i);
        // A dead stdout (`seq N | head -1`) stops the generator instead of
        // spinning a failing write per remaining value -- the EPIPE pipe-note
        // never terminates a non-reading native Proc (RW-9 R4-F3).
        if out.write_all(buf.as_bytes()).is_err() {
            break;
        }
        match i.checked_add(incr) {
            Some(n) => i = n,
            None => break, // would overflow past the i64 boundary
        }
    }
    0
}
