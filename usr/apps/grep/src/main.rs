// grep [-ivnc] PATTERN [FILE...] -- print lines matching PATTERN.
//
// "simple" per the roadmap: PATTERN is a LITERAL substring, not a regex
// (DOC-GAP-side: no regex engine in libthyla-rs). -i case-insensitive,
// -v invert, -n line numbers, -c count-only. No operand FILE reads stdin.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::fs::File;

aux_rt::main!(run);

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

fn grep_data(data: &[u8], pat: &[u8], f: &Flags, prefix: Option<&str>) -> usize {
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
                    aux_rt::print!("{}:", p);
                }
                if f.number {
                    aux_rt::print!("{}:", n + 1);
                }
                aux_rt::out(line);
                aux_rt::out(b"\n");
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
                        aux_rt::eprintln!("grep: invalid option -- '{}'", ch);
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
            aux_rt::eprintln!("grep: missing pattern");
            return 2;
        }
    };
    idx += 1;

    // Collect file operands.
    let mut files: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => files.push(p),
            Err(_) => {
                aux_rt::eprintln!("grep: invalid UTF-8 path");
                return 2;
            }
        }
    }

    let multi = files.len() > 1;
    let mut any_match = false;
    let mut status_err = false;

    if files.is_empty() {
        match aux_rt::slurp(&mut aux_rt::stdin()) {
            Ok(data) => {
                let m = grep_data(&data, pat, &f, None);
                if f.count {
                    aux_rt::println!("{}", m);
                }
                any_match |= m > 0;
            }
            Err(e) => {
                aux_rt::eprintln!("grep: stdin: {}", e);
                status_err = true;
            }
        }
    } else {
        for path in files {
            match File::open(path).and_then(|mut fh| aux_rt::slurp(&mut fh)) {
                Ok(data) => {
                    let prefix = if multi { Some(path) } else { None };
                    let m = grep_data(&data, pat, &f, prefix);
                    if f.count {
                        if multi {
                            aux_rt::println!("{}:{}", path, m);
                        } else {
                            aux_rt::println!("{}", m);
                        }
                    }
                    any_match |= m > 0;
                }
                Err(e) => {
                    aux_rt::eprintln!("grep: {}: {}", path, e);
                    status_err = true;
                }
            }
        }
    }

    if status_err {
        2
    } else if any_match {
        0
    } else {
        1
    }
}
