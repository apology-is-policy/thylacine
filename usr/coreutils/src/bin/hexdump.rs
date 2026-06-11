// hexdump [FILE...] -- canonical hex+ASCII dump (like `hexdump -C` / xxd).
//
// 16 bytes per line: 8-digit offset, two 8-byte hex groups, then the ASCII
// gutter. No operands (or "-") dumps stdin. Streams in 16-byte rows.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::fmt::Write as _;
use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Result;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, Read};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn read_full<R: Read>(r: &mut R, buf: &mut [u8]) -> Result<usize> {
    let mut got = 0;
    while got < buf.len() {
        match r.read(&mut buf[got..])? {
            0 => break,
            n => got += n,
        }
    }
    Ok(got)
}

fn dump<R: Read>(out: &mut io::OutSink, r: &mut R) -> Result<()> {
    let mut buf = [0u8; 16];
    let mut offset: u64 = 0;
    loop {
        let n = read_full(r, &mut buf)?;
        if n == 0 {
            break;
        }
        let _ = write!(out, "{:08x}  ", offset);
        for (i, &byte) in buf.iter().enumerate() {
            if i < n {
                let _ = write!(out, "{:02x} ", byte);
            } else {
                out.put(b"   ");
            }
            if i == 7 {
                out.put(b" ");
            }
        }
        out.put(b" |");
        for &b in &buf[..n] {
            let pc = if (0x20..0x7f).contains(&b) { b } else { b'.' };
            out.put(&[pc]);
        }
        out.put(b"|\n");
        offset += n as u64;
    }
    let _ = write!(out, "{:08x}\n", offset);
    Ok(())
}

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut out = io::OutSink::new();
    let mut had = false;
    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("hexdump: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if path == "-" {
            if let Err(e) = dump(&mut out, &mut io::stdin()) {
                eprintln!("hexdump: -: {}", e);
                status = 1;
            }
            continue;
        }
        match File::open(path) {
            Ok(mut f) => {
                if let Err(e) = dump(&mut out, &mut f) {
                    eprintln!("hexdump: {}: {}", path, e);
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("hexdump: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        if let Err(e) = dump(&mut out, &mut io::stdin()) {
            eprintln!("hexdump: stdin: {}", e);
            status = 1;
        }
    }
    if out.failed() {
        eprintln!("hexdump: write error");
        return 1;
    }
    status
}
