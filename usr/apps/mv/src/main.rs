// mv SRC... DST -- move/rename.
//
// Tries the atomic aux-rt::fs::rename first. If that fails (e.g. the two
// ends are on different Dev/sessions, which rename forbids), falls back to
// copy+unlink FOR REGULAR FILES; a cross-session directory move is a v1.0
// limitation. SRC... DST with DST a directory moves each SRC into it.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::err::Error;
use libthyla_rs::fs::File;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut idx = 1;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        // Accept and ignore single-letter flags like -f / -v (no prompting
        // or verbosity at v1); stop at the first non-flag.
        if a.starts_with('-') && a != "-" {
            idx += 1;
        } else {
            break;
        }
    }

    let mut ops: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => ops.push(p),
            Err(_) => {
                aux_rt::eprintln!("mv: invalid UTF-8 path");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        aux_rt::eprintln!("mv: missing file operand");
        return 1;
    }
    let dst = ops[ops.len() - 1];
    let srcs = &ops[..ops.len() - 1];
    let dst_is_dir = libthyla_rs::fs::is_dir(dst);
    if srcs.len() > 1 && !dst_is_dir {
        aux_rt::eprintln!("mv: target '{}' is not a directory", dst);
        return 1;
    }

    let mut status = 0;
    for &src in srcs {
        let target = if dst_is_dir {
            join(dst, base(src))
        } else {
            dst.to_string()
        };
        if let Err(e) = move_one(src, &target) {
            aux_rt::eprintln!("mv: {}: {}", src, e);
            status = 1;
        }
    }
    status
}

fn move_one(src: &str, dst: &str) -> aux_rt::Result<()> {
    match aux_rt::fs::rename(src, dst) {
        Ok(()) => Ok(()),
        Err(_) => {
            // Cross-session fallback: copy then unlink (regular files only).
            let m = libthyla_rs::fs::metadata(src)?;
            if m.is_dir() {
                return Err(Error::InvalidArgument); // cross-session dir move: v1 limitation
            }
            let mut r = File::open(src)?;
            let mut w = aux_rt::fs::create(dst, 0o644)?;
            aux_rt::copy(&mut r, &mut w)?;
            aux_rt::fs::remove_file(src)?;
            Ok(())
        }
    }
}

fn base(p: &str) -> &str {
    let t = p.trim_end_matches('/');
    match t.rfind('/') {
        Some(i) => &t[i + 1..],
        None => t,
    }
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
