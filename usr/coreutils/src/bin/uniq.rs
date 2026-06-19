// uniq [-cdui] [FILE] -- collapse ADJACENT duplicate lines.
//
// -c prefixes each output line with its run count. -d prints only duplicated
// groups (count > 1); -u prints only unique lines (count == 1); -i compares
// case-insensitively (the first line of each group is emitted as-is). Reads one
// FILE (or stdin). Like GNU uniq, only ADJACENT duplicates are merged (sort
// first to dedupe globally).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::fmt::Write as _;
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn emit(out: &mut io::OutSink, line: &[u8], count: usize, with_count: bool) {
    if with_count {
        let _ = write!(out, "{:>7} ", count);
    }
    out.put(line);
    out.put(b"\n");
}

/// Adjacent-line equality, optionally ASCII-case-insensitive.
fn same(a: &[u8], b: &[u8], ignore_case: bool) -> bool {
    if ignore_case {
        a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.eq_ignore_ascii_case(y))
    } else {
        a == b
    }
}

const USAGE: &str = "\
usage: uniq [-cdui] [FILE]
  Collapse adjacent duplicate lines.
  -c      prefix each line with its run count
  -d      only print duplicated lines (count > 1)
  -u      only print unique lines (count == 1)
  -i      ignore case when comparing
  --help  show this help

Examples:
  sort f | uniq         # drop adjacent duplicates
  sort f | uniq -c      # with a run count
  uniq -d file          # only the duplicated lines
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut with_count = false;
    let mut only_dup = false;
    let mut only_uniq = false;
    let mut ignore_case = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'c' => with_count = true,
                    'd' => only_dup = true,
                    'u' => only_uniq = true,
                    'i' => ignore_case = true,
                    _ => {
                        eprintln!("uniq: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    // Whether a group of `count` lines should be emitted. -d and -u together
    // select nothing (a line cannot be both duplicated and unique).
    let want = |count: usize| match (only_dup, only_uniq) {
        (true, true) => false,
        (true, false) => count > 1,
        (false, true) => count == 1,
        (false, false) => true,
    };

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

    let mut out = io::OutSink::new();
    let mut prev: Option<&[u8]> = None;
    let mut count = 0usize;
    for line in lines {
        match prev {
            Some(p) if same(p, line, ignore_case) => count += 1,
            Some(p) => {
                if want(count) {
                    emit(&mut out, p, count, with_count);
                }
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
        if want(count) {
            emit(&mut out, p, count, with_count);
        }
    }
    if out.failed() {
        eprintln!("uniq: write error");
        return 1;
    }
    0
}
