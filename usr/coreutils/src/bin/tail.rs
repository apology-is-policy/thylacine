// tail [-n N | -N] [FILE...] -- print the last N lines (default 10).
//
// Reads each input fully (it cannot know the last N lines without seeing
// the end), then scans backward for the N-th line boundary. A trailing
// newline does NOT count as an extra empty line. Multiple files get
// "==> name <==" banners. No operand / "-" reads stdin.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Result;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Write};
use libthyla_rs::{eprintln, print};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: tail [-n N | -c N | -N] [FILE...]
  Print the last N lines (default 10), or last N bytes with -c.
  -n N    last N lines        -c N    last N bytes
  --help  show this help

Examples:
  tail file             # last 10 lines
  tail -n 5 file        # last 5 lines
  tail -c 20 file       # last 20 bytes
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut n: usize = 10;
    let mut bytes = false;
    let mut idx = 1;
    loop {
        match args.get_str(idx) {
            Some("-n") | Some("-c") => {
                let want_bytes = args.get_str(idx) == Some("-c");
                match args.get_str(idx + 1).and_then(|s| s.parse::<usize>().ok()) {
                    Some(v) => {
                        n = v;
                        bytes = want_bytes;
                        idx += 2;
                    }
                    None => {
                        eprintln!("tail: invalid count");
                        return 1;
                    }
                }
            }
            // Attached forms `-cN` / `-nN`.
            Some(a)
                if a.len() > 2
                    && (a.starts_with("-c") || a.starts_with("-n"))
                    && a[2..].bytes().all(|c| c.is_ascii_digit()) =>
            {
                n = a[2..].parse::<usize>().unwrap_or(10);
                bytes = a.starts_with("-c");
                idx += 1;
            }
            // Legacy `-N` = last N lines.
            Some(a)
                if a.len() > 1
                    && a.starts_with('-')
                    && a != "-"
                    && a[1..].bytes().all(|c| c.is_ascii_digit()) =>
            {
                n = a[1..].parse::<usize>().unwrap_or(10);
                bytes = false;
                idx += 1;
            }
            _ => break,
        }
    }

    let mut count_ops = 0usize;
    {
        let mut k = idx;
        while args.get(k).is_some() {
            count_ops += 1;
            k += 1;
        }
    }

    let mut status = 0;
    let mut had = false;
    let mut first = true;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("tail: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if count_ops > 1 {
            if !first {
                io::out(b"\n");
            }
            print!("==> {} <==\n", path);
            first = false;
        }
        match File::open(path).and_then(|mut f| io::slurp(&mut f)) {
            Ok(data) => {
                if let Err(e) = emit(&data, n, bytes) {
                    eprintln!("tail: {}: {}", path, e);
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("tail: {}: {}", path, e);
                status = 1;
            }
        }
    }

    if !had {
        match io::slurp(&mut io::stdin()) {
            Ok(data) => {
                if let Err(e) = emit(&data, n, bytes) {
                    eprintln!("tail: stdin: {}", e);
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("tail: stdin: {}", e);
                status = 1;
            }
        }
    }
    status
}

fn emit(data: &[u8], n: usize, bytes: bool) -> Result<()> {
    if bytes {
        emit_tail_bytes(data, n)
    } else {
        emit_tail_lines(data, n)
    }
}

fn emit_tail_bytes(data: &[u8], n: usize) -> Result<()> {
    if n == 0 {
        return Ok(());
    }
    let start = data.len().saturating_sub(n);
    io::stdout().write_all(&data[start..])
}

fn emit_tail_lines(data: &[u8], n: usize) -> Result<()> {
    if n == 0 || data.is_empty() {
        return Ok(());
    }
    // Ignore a single trailing newline so it does not create a phantom line.
    let end = if *data.last().unwrap() == b'\n' {
        data.len() - 1
    } else {
        data.len()
    };
    let mut count = 0usize;
    let mut start = 0usize;
    let mut j = end;
    while j > 0 {
        j -= 1;
        if data[j] == b'\n' {
            count += 1;
            if count == n {
                start = j + 1;
                break;
            }
        }
    }
    io::stdout().write_all(&data[start..])
}
