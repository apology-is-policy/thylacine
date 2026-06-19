// wc [-clwm] [FILE...] -- count lines, words, bytes.
//
// Default output: lines, words, bytes (GNU order), each in a 7-wide field,
// then the name. Multiple files add a "total" line. Flags select a subset:
// -l lines, -w words, -c bytes, -m chars (approximated as bytes at v1 --
// no multibyte awareness). Reads each file fully via io::slurp.

#![no_std]
#![no_main]

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

struct Counts {
    lines: usize,
    words: usize,
    bytes: usize,
}

fn count(data: &[u8]) -> Counts {
    let bytes = data.len();
    let mut lines = 0usize;
    let mut words = 0usize;
    let mut in_word = false;
    for &b in data {
        if b == b'\n' {
            lines += 1;
        }
        let ws = matches!(b, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c);
        if ws {
            in_word = false;
        } else if !in_word {
            words += 1;
            in_word = true;
        }
    }
    Counts { lines, words, bytes }
}

fn print_counts(out: &mut io::OutSink, c: &Counts, l: bool, w: bool, by: bool, name: Option<&str>) {
    if l {
        let _ = write!(out, "{:>7}", c.lines);
    }
    if w {
        let _ = write!(out, "{:>7}", c.words);
    }
    if by {
        let _ = write!(out, "{:>7}", c.bytes);
    }
    if let Some(nm) = name {
        let _ = write!(out, " {}", nm);
    }
    out.put(b"\n");
}

const USAGE: &str = "\
usage: wc [-clwm] [FILE...]
  Count lines, words, and bytes of each FILE (or stdin). Default prints all
  three; flags select a subset.
  -l  lines    -w  words    -c  bytes    -m  chars (= bytes at v1.0)
  --help  show this help

Examples:
  wc file               # lines words bytes file
  wc -l file            # just the line count
  ls | wc -l            # count entries
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let (mut want_l, mut want_w, mut want_c) = (false, false, false);
    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        if a.len() >= 2 && a.starts_with('-') && a != "-" {
            for ch in a[1..].chars() {
                match ch {
                    'l' => want_l = true,
                    'w' => want_w = true,
                    'c' | 'm' => want_c = true,
                    _ => {
                        eprintln!("wc: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }
    if !(want_l || want_w || want_c) {
        want_l = true;
        want_w = true;
        want_c = true;
    }

    let mut total = Counts { lines: 0, words: 0, bytes: 0 };
    let mut nfiles = 0usize;
    let mut status = 0;
    let mut out = io::OutSink::new();
    let mut had = false;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("wc: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        match File::open(path).and_then(|mut f| io::slurp(&mut f)) {
            Ok(data) => {
                let c = count(&data);
                print_counts(&mut out, &c, want_l, want_w, want_c, Some(path));
                total.lines += c.lines;
                total.words += c.words;
                total.bytes += c.bytes;
                nfiles += 1;
            }
            Err(e) => {
                eprintln!("wc: {}: {}", path, e);
                status = 1;
            }
        }
    }

    if !had {
        match io::slurp(&mut io::stdin()) {
            Ok(data) => {
                let c = count(&data);
                print_counts(&mut out, &c, want_l, want_w, want_c, None);
            }
            Err(e) => {
                eprintln!("wc: stdin: {}", e);
                status = 1;
            }
        }
    } else if nfiles > 1 {
        print_counts(&mut out, &total, want_l, want_w, want_c, Some("total"));
    }
    if out.failed() {
        eprintln!("wc: write error");
        return 1;
    }
    status
}
