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

use aux_rt::Args;
use libthyla_rs::fs::File;

aux_rt::main!(run);

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

fn cut_line_fields(line: &[u8], delim: u8, ranges: &[(usize, usize)]) {
    let fields: Vec<&[u8]> = line.split(|&b| b == delim).collect();
    // If there is no delimiter, GNU cut prints the whole line unchanged.
    if fields.len() == 1 {
        aux_rt::out(line);
        aux_rt::out(b"\n");
        return;
    }
    let mut first = true;
    for (i, fld) in fields.iter().enumerate() {
        if selected(i + 1, ranges) {
            if !first {
                aux_rt::out(&[delim]);
            }
            aux_rt::out(fld);
            first = false;
        }
    }
    aux_rt::out(b"\n");
}

fn cut_line_chars(line: &[u8], ranges: &[(usize, usize)]) {
    for (i, &b) in line.iter().enumerate() {
        if selected(i + 1, ranges) {
            aux_rt::out(&[b]);
        }
    }
    aux_rt::out(b"\n");
}

fn process(data: &[u8], by_field: bool, delim: u8, ranges: &[(usize, usize)]) {
    let mut lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    if data.last() == Some(&b'\n') {
        lines.pop();
    }
    for line in lines {
        if by_field {
            cut_line_fields(line, delim, ranges);
        } else {
            cut_line_chars(line, ranges);
        }
    }
}

fn run(args: Args) -> i64 {
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
            // -d, or -dDELIM
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
            aux_rt::eprintln!("cut: invalid option {}", a);
            return 1;
        } else {
            break;
        }
    }

    let (by_field, list_str) = match (flist, clist) {
        (Some(l), None) => (true, l),
        (None, Some(l)) => (false, l),
        _ => {
            aux_rt::eprintln!("cut: specify exactly one of -f or -c");
            return 1;
        }
    };
    let ranges = match parse_list(list_str) {
        Some(r) => r,
        None => {
            aux_rt::eprintln!("cut: invalid list '{}'", list_str);
            return 1;
        }
    };

    let mut status = 0;
    let mut had = false;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                aux_rt::eprintln!("cut: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        match File::open(path).and_then(|mut f| aux_rt::slurp(&mut f)) {
            Ok(data) => process(&data, by_field, delim, &ranges),
            Err(e) => {
                aux_rt::eprintln!("cut: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        match aux_rt::slurp(&mut aux_rt::stdin()) {
            Ok(data) => process(&data, by_field, delim, &ranges),
            Err(e) => {
                aux_rt::eprintln!("cut: stdin: {}", e);
                status = 1;
            }
        }
    }
    status
}
