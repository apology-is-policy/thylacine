// hexdump [FILE...] -- canonical hex+ASCII dump (like `hexdump -C` / xxd).
//
// 16 bytes per line: 8-digit offset, two 8-byte hex groups, then the ASCII
// gutter. No operands (or "-") dumps stdin. Streams in 16-byte rows.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::{Args, Read};
use libthyla_rs::fs::File;

aux_rt::main!(run);

fn read_full<R: Read>(r: &mut R, buf: &mut [u8]) -> aux_rt::Result<usize> {
    let mut got = 0;
    while got < buf.len() {
        match r.read(&mut buf[got..])? {
            0 => break,
            n => got += n,
        }
    }
    Ok(got)
}

fn dump<R: Read>(r: &mut R) -> aux_rt::Result<()> {
    let mut buf = [0u8; 16];
    let mut offset: u64 = 0;
    loop {
        let n = read_full(r, &mut buf)?;
        if n == 0 {
            break;
        }
        aux_rt::print!("{:08x}  ", offset);
        for (i, &byte) in buf.iter().enumerate() {
            if i < n {
                aux_rt::print!("{:02x} ", byte);
            } else {
                aux_rt::out(b"   ");
            }
            if i == 7 {
                aux_rt::out(b" ");
            }
        }
        aux_rt::out(b" |");
        for &b in &buf[..n] {
            let pc = if (0x20..0x7f).contains(&b) { b } else { b'.' };
            aux_rt::out(&[pc]);
        }
        aux_rt::out(b"|\n");
        offset += n as u64;
    }
    aux_rt::print!("{:08x}\n", offset);
    Ok(())
}

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                aux_rt::eprintln!("hexdump: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if path == "-" {
            if let Err(e) = dump(&mut aux_rt::stdin()) {
                aux_rt::eprintln!("hexdump: -: {}", e);
                status = 1;
            }
            continue;
        }
        match File::open(path) {
            Ok(mut f) => {
                if let Err(e) = dump(&mut f) {
                    aux_rt::eprintln!("hexdump: {}: {}", path, e);
                    status = 1;
                }
            }
            Err(e) => {
                aux_rt::eprintln!("hexdump: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        if let Err(e) = dump(&mut aux_rt::stdin()) {
            aux_rt::eprintln!("hexdump: stdin: {}", e);
            status = 1;
        }
    }
    status
}
