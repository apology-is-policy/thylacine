// grep [-ivnc] PATTERN [FILE...] -- print lines matching PATTERN.
//
// "simple" per the roadmap: PATTERN is a LITERAL substring, not a regex (no
// regex engine in libthyla-rs). -i case-insensitive, -v invert, -n line
// numbers, -c count-only. No operand FILE reads stdin.

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

fn contains(hay: &[u8], needle: &[u8], ci: bool) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    hay.windows(needle.len()).any(|w| {
        if ci {
            w.iter().zip(needle).all(|(a, b)| a.eq_ignore_ascii_case(b))
        } else {
            w == needle
        }
    })
}

struct Flags {
    ci: bool,
    invert: bool,
    number: bool,
    count: bool,
}

fn grep_data(out: &mut io::OutSink, data: &[u8], pat: &[u8], f: &Flags, prefix: Option<&str>) -> usize {
    let mut lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    if data.last() == Some(&b'\n') {
        lines.pop();
    }
    let mut matches = 0usize;
    for (n, line) in lines.iter().enumerate() {
        if contains(line, pat, f.ci) != f.invert {
            matches += 1;
            if !f.count {
                if let Some(p) = prefix {
                    let _ = write!(out, "{}:", p);
                }
                if f.number {
                    let _ = write!(out, "{}:", n + 1);
                }
                out.put(line);
                out.put(b"\n");
            }
        }
    }
    matches
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut f = Flags {
        ci: false,
        invert: false,
        number: false,
        count: false,
    };
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'i' => f.ci = true,
                    'v' => f.invert = true,
                    'n' => f.number = true,
                    'c' => f.count = true,
                    _ => {
                        eprintln!("grep: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let pat = match args.get(idx) {
        Some(p) => p,
        None => {
            eprintln!("grep: missing pattern");
            return 2;
        }
    };
    idx += 1;

    let mut files: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => files.push(p),
            Err(_) => {
                eprintln!("grep: invalid UTF-8 path");
                return 2;
            }
        }
    }

    let multi = files.len() > 1;
    let mut any_match = false;
    let mut status_err = false;
    let mut out = io::OutSink::new();

    if files.is_empty() {
        match io::slurp(&mut io::stdin()) {
            Ok(data) => {
                let m = grep_data(&mut out, &data, pat, &f, None);
                if f.count {
                    let _ = writeln!(out, "{}", m);
                }
                any_match |= m > 0;
            }
            Err(e) => {
                eprintln!("grep: stdin: {}", e);
                status_err = true;
            }
        }
    } else {
        for path in files {
            match File::open(path).and_then(|mut fh| io::slurp(&mut fh)) {
                Ok(data) => {
                    let prefix = if multi { Some(path) } else { None };
                    let m = grep_data(&mut out, &data, pat, &f, prefix);
                    if f.count {
                        if multi {
                            let _ = writeln!(out, "{}:{}", path, m);
                        } else {
                            let _ = writeln!(out, "{}", m);
                        }
                    }
                    any_match |= m > 0;
                }
                Err(e) => {
                    eprintln!("grep: {}: {}", path, e);
                    status_err = true;
                }
            }
        }
    }

    if out.failed() {
        eprintln!("grep: write error");
        status_err = true;
    }

    if status_err {
        2
    } else if any_match {
        0
    } else {
        1
    }
}
