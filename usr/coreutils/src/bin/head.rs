// head [-n N | -N] [FILE...] -- print the first N lines (default 10).
//
// Streams: stops reading after the N-th newline, so it does not slurp a
// whole large file or block forever on an endless stream. Multiple files
// get "==> name <==" banners (GNU style). No operand / "-" reads stdin.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Result;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::{eprintln, print};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut n: usize = 10;
    let mut idx = 1;
    loop {
        match args.get_str(idx) {
            Some("-n") => match args.get_str(idx + 1).and_then(|s| s.parse::<usize>().ok()) {
                Some(v) => {
                    n = v;
                    idx += 2;
                }
                None => {
                    eprintln!("head: invalid number of lines");
                    return 1;
                }
            },
            Some(a)
                if a.len() > 1
                    && a.starts_with('-')
                    && a != "-"
                    && a[1..].bytes().all(|c| c.is_ascii_digit()) =>
            {
                n = a[1..].parse::<usize>().unwrap_or(10);
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
                eprintln!("head: invalid UTF-8 path");
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
        match File::open(path) {
            Ok(mut f) => {
                if let Err(e) = emit_head(&mut f, n) {
                    eprintln!("head: {}: {}", path, e);
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("head: {}: {}", path, e);
                status = 1;
            }
        }
    }

    if !had {
        if let Err(e) = emit_head(&mut io::stdin(), n) {
            eprintln!("head: stdin: {}", e);
            status = 1;
        }
    }
    status
}

fn emit_head<R: Read>(r: &mut R, n: usize) -> Result<()> {
    if n == 0 {
        return Ok(());
    }
    let mut out = io::stdout();
    let mut buf = [0u8; 4096];
    let mut lines = 0usize;
    loop {
        let got = r.read(&mut buf)?;
        if got == 0 {
            return Ok(());
        }
        let chunk = &buf[..got];
        let mut emit_to = got;
        let mut done = false;
        for (j, &b) in chunk.iter().enumerate() {
            if b == b'\n' {
                lines += 1;
                if lines == n {
                    emit_to = j + 1;
                    done = true;
                    break;
                }
            }
        }
        out.write_all(&chunk[..emit_to])?;
        if done {
            return Ok(());
        }
    }
}
