// uniq [-c] [FILE] -- collapse ADJACENT duplicate lines.
//
// -c prefixes each output line with its run count. Reads one FILE (or
// stdin). Like GNU uniq, only ADJACENT duplicates are merged (sort first to
// dedupe globally).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io, print};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn emit(line: &[u8], count: usize, with_count: bool) {
    if with_count {
        print!("{:>7} ", count);
    }
    io::out(line);
    io::out(b"\n");
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut with_count = false;
    while let Some(a) = args.get_str(idx) {
        match a {
            "-c" => {
                with_count = true;
                idx += 1;
            }
            "--" => {
                idx += 1;
                break;
            }
            _ if a.starts_with('-') && a != "-" => {
                eprintln!("uniq: unknown option {}", a);
                return 1;
            }
            _ => break,
        }
    }

    let data = match args.get(idx) {
        Some(op) => {
            let path = match core::str::from_utf8(op) {
                Ok(p) => p,
                Err(_) => {
                    eprintln!("uniq: invalid UTF-8 path");
                    return 1;
                }
            };
            match File::open(path).and_then(|mut f| io::slurp(&mut f)) {
                Ok(d) => d,
                Err(e) => {
                    eprintln!("uniq: {}: {}", path, e);
                    return 1;
                }
            }
        }
        None => match io::slurp(&mut io::stdin()) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("uniq: stdin: {}", e);
                return 1;
            }
        },
    };

    let mut lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    if data.last() == Some(&b'\n') {
        lines.pop(); // drop the spurious empty after a trailing newline
    }

    let mut prev: Option<&[u8]> = None;
    let mut count = 0usize;
    for line in lines {
        match prev {
            Some(p) if p == line => count += 1,
            Some(p) => {
                emit(p, count, with_count);
                prev = Some(line);
                count = 1;
            }
            None => {
                prev = Some(line);
                count = 1;
            }
        }
    }
    if let Some(p) = prev {
        emit(p, count, with_count);
    }
    0
}
