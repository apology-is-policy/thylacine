// sort [-rnu] [FILE...] -- sort lines (lexical, or -n numeric); -r reverse,
// -u drop adjacent-after-sort duplicates. Reads all inputs into memory.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn num_key(line: &[u8]) -> i64 {
    let s = core::str::from_utf8(line).unwrap_or("");
    let t = s.trim_start();
    let b = t.as_bytes();
    let mut end = 0;
    if end < b.len() && (b[end] == b'-' || b[end] == b'+') {
        end += 1;
    }
    while end < b.len() && b[end].is_ascii_digit() {
        end += 1;
    }
    t[..end].parse::<i64>().unwrap_or(0)
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let (mut rev, mut num, mut uniq) = (false, false, false);
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'r' => rev = true,
                    'n' => num = true,
                    'u' => uniq = true,
                    _ => {
                        eprintln!("sort: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut data: Vec<u8> = Vec::new();
    let mut had = false;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("sort: invalid UTF-8 path");
                return 1;
            }
        };
        match File::open(path).and_then(|mut f| io::slurp(&mut f)) {
            Ok(d) => data.extend_from_slice(&d),
            Err(e) => {
                eprintln!("sort: {}: {}", path, e);
                return 1;
            }
        }
    }
    if !had {
        match io::slurp(&mut io::stdin()) {
            Ok(d) => data = d,
            Err(e) => {
                eprintln!("sort: stdin: {}", e);
                return 1;
            }
        }
    }

    let mut lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    if data.last() == Some(&b'\n') {
        lines.pop();
    }
    if num {
        lines.sort_unstable_by(|a, b| num_key(a).cmp(&num_key(b)).then_with(|| a.cmp(b)));
    } else {
        lines.sort_unstable();
    }
    if rev {
        lines.reverse();
    }

    let mut out = io::OutSink::new();
    let mut prev: Option<&[u8]> = None;
    for line in lines {
        if uniq {
            if prev == Some(line) {
                continue;
            }
            prev = Some(line);
        }
        out.put(line);
        out.put(b"\n");
    }
    if out.failed() {
        eprintln!("sort: write error");
        return 1;
    }
    0
}
