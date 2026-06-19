// cut -f LIST [-d DELIM] [FILE...]  /  cut -c LIST [FILE...]
//
// -f LIST: select delimiter-separated fields (default delim TAB).
// -c LIST: select 1-based byte positions.
// LIST is comma-separated single positions and N-M / N- / -M ranges.
// Reads each FILE (or stdin), one transformed line at a time.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

// Inclusive 1-based range; hi == usize::MAX means open-ended.
fn parse_list(s: &str) -> Option<Vec<(usize, usize)>> {
    let mut out = Vec::new();
    for part in s.split(',') {
        if part.is_empty() {
            return None;
        }
        if let Some((a, b)) = part.split_once('-') {
            let lo = if a.is_empty() { 1 } else { a.parse().ok()? };
            let hi = if b.is_empty() { usize::MAX } else { b.parse().ok()? };
            if lo == 0 || hi < lo {
                return None;
            }
            out.push((lo, hi));
        } else {
            let n: usize = part.parse().ok()?;
            if n == 0 {
                return None;
            }
            out.push((n, n));
        }
    }
    Some(out)
}

fn selected(pos: usize, ranges: &[(usize, usize)]) -> bool {
    ranges.iter().any(|&(lo, hi)| pos >= lo && pos <= hi)
}

fn cut_line_fields(out: &mut io::OutSink, line: &[u8], delim: u8, ranges: &[(usize, usize)]) {
    let fields: Vec<&[u8]> = line.split(|&b| b == delim).collect();
    // If there is no delimiter, GNU cut prints the whole line unchanged.
    if fields.len() == 1 {
        out.put(line);
        out.put(b"\n");
        return;
    }
    let mut first = true;
    for (i, fld) in fields.iter().enumerate() {
        if selected(i + 1, ranges) {
            if !first {
                out.put(&[delim]);
            }
            out.put(fld);
            first = false;
        }
    }
    out.put(b"\n");
}

fn cut_line_chars(out: &mut io::OutSink, line: &[u8], ranges: &[(usize, usize)]) {
    for (i, &b) in line.iter().enumerate() {
        if selected(i + 1, ranges) {
            out.put(&[b]);
        }
    }
    out.put(b"\n");
}

fn process(out: &mut io::OutSink, data: &[u8], by_field: bool, delim: u8, ranges: &[(usize, usize)]) {
    let mut lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    if data.last() == Some(&b'\n') {
        lines.pop();
    }
    for line in lines {
        if by_field {
            cut_line_fields(out, line, delim, ranges);
        } else {
            cut_line_chars(out, line, ranges);
        }
    }
}

const USAGE: &str = "\
usage: cut -f LIST [-d DELIM] [FILE...]
       cut -c LIST [FILE...]
  Print selected parts of each line. LIST is comma-separated positions and
  ranges (1,3 or 2-5 or 4- or -3).
  -f LIST   select delimiter-separated fields
  -c LIST   select 1-based byte positions
  -d DELIM  field delimiter for -f (default TAB)
  --help    show this help

Examples:
  cut -d: -f1 /etc/passwd   # first colon-field of each line
  cut -c1-3 file            # first three bytes of each line
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut delim = b'\t';
    let mut flist: Option<&str> = None;
    let mut clist: Option<&str> = None;

    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if let Some(rest) = a.strip_prefix("-d") {
            let d = if rest.is_empty() {
                idx += 1;
                args.get_str(idx).unwrap_or("\t")
            } else {
                rest
            };
            delim = d.as_bytes().first().copied().unwrap_or(b'\t');
            idx += 1;
        } else if let Some(rest) = a.strip_prefix("-f") {
            flist = Some(if rest.is_empty() {
                idx += 1;
                args.get_str(idx).unwrap_or("")
            } else {
                rest
            });
            idx += 1;
        } else if let Some(rest) = a.strip_prefix("-c") {
            clist = Some(if rest.is_empty() {
                idx += 1;
                args.get_str(idx).unwrap_or("")
            } else {
                rest
            });
            idx += 1;
        } else if a.starts_with('-') && a != "-" {
            eprintln!("cut: invalid option {}", a);
            return 1;
        } else {
            break;
        }
    }

    let (by_field, list_str) = match (flist, clist) {
        (Some(l), None) => (true, l),
        (None, Some(l)) => (false, l),
        _ => {
            eprintln!("cut: specify exactly one of -f or -c");
            return 1;
        }
    };
    let ranges = match parse_list(list_str) {
        Some(r) => r,
        None => {
            eprintln!("cut: invalid list '{}'", list_str);
            return 1;
        }
    };

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
                eprintln!("cut: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        match File::open(path).and_then(|mut f| io::slurp(&mut f)) {
            Ok(data) => process(&mut out, &data, by_field, delim, &ranges),
            Err(e) => {
                eprintln!("cut: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        match io::slurp(&mut io::stdin()) {
            Ok(data) => process(&mut out, &data, by_field, delim, &ranges),
            Err(e) => {
                eprintln!("cut: stdin: {}", e);
                status = 1;
            }
        }
    }
    if out.failed() {
        eprintln!("cut: write error");
        return 1;
    }
    status
}
