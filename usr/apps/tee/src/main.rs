// tee [-a] [FILE...] -- copy stdin to stdout AND to each FILE.
//
// First app to use the aux-rt::fs create shim (DOC-GAP G09: the safe fs API
// cannot create files). Each FILE is created/truncated (mode 0644). `-a`
// (append) is accepted but DEGRADES to truncate at v1.0 -- append has no
// kernel/fs surface yet (G09); a warning is printed so the behavior is not
// silent.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::fs::OwnedFd;
use aux_rt::{Args, Read, Write};

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut append = false;
    while let Some(a) = args.get_str(idx) {
        match a {
            "-a" => {
                append = true;
                idx += 1;
            }
            "--" => {
                idx += 1;
                break;
            }
            _ if a.starts_with('-') && a != "-" => {
                aux_rt::eprintln!("tee: unknown option {}", a);
                return 1;
            }
            _ => break,
        }
    }
    if append {
        aux_rt::eprintln!("tee: -a (append) unsupported at v1.0; truncating instead");
    }

    let mut status = 0;
    let mut files: Vec<OwnedFd> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                aux_rt::eprintln!("tee: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        match aux_rt::fs::create(path, 0o644) {
            Ok(f) => files.push(f),
            Err(e) => {
                aux_rt::eprintln!("tee: {}: {}", path, e);
                status = 1;
            }
        }
    }

    let mut inp = aux_rt::stdin();
    let mut out = aux_rt::stdout();
    let mut buf = [0u8; 4096];
    loop {
        let n = match inp.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => {
                aux_rt::eprintln!("tee: stdin: {}", e);
                status = 1;
                break;
            }
        };
        let chunk = &buf[..n];
        let _ = out.write_all(chunk); // best-effort stdout (may be unwired; G06)
        for f in files.iter_mut() {
            if let Err(e) = f.write_all(chunk) {
                aux_rt::eprintln!("tee: write: {}", e);
                status = 1;
            }
        }
    }
    status
}
