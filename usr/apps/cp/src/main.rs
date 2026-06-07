// cp [-r] SRC... DST -- copy files (and, with -r, directory trees).
//
// SRC... DST where DST is a directory copies each SRC into it (by basename).
// Reads via libthyla-rs::fs::File, writes via aux-rt::fs::create. Does not
// preserve mode/owner/timestamps (no metadata-set surface beyond mode at
// create; timestamps unsupported, G12).

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
    let mut recursive = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'r' | 'R' => recursive = true,
                    _ => {
                        aux_rt::eprintln!("cp: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
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
                aux_rt::eprintln!("cp: invalid UTF-8 path");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        aux_rt::eprintln!("cp: missing file operand");
        return 1;
    }
    let dst = ops[ops.len() - 1];
    let srcs = &ops[..ops.len() - 1];
    let dst_is_dir = libthyla_rs::fs::is_dir(dst);
    if srcs.len() > 1 && !dst_is_dir {
        aux_rt::eprintln!("cp: target '{}' is not a directory", dst);
        return 1;
    }

    let mut status = 0;
    for &src in srcs {
        let target = if dst_is_dir {
            join(dst, base(src))
        } else {
            dst.to_string()
        };
        if let Err(e) = copy_any(src, &target, recursive) {
            aux_rt::eprintln!("cp: {}: {}", src, e);
            status = 1;
        }
    }
    status
}

fn copy_any(src: &str, dst: &str, recursive: bool) -> aux_rt::Result<()> {
    let m = libthyla_rs::fs::metadata(src)?;
    if m.is_dir() {
        if !recursive {
            return Err(Error::InvalidArgument); // omitting directory (no -r)
        }
        match aux_rt::fs::mkdir(dst, 0o755) {
            Ok(()) | Err(Error::Exists) => {}
            Err(e) => return Err(e),
        }
        for e in aux_rt::fs::read_dir(src)? {
            if e.name == "." || e.name == ".." {
                continue;
            }
            let s = join(src, &e.name);
            let d = join(dst, &e.name);
            copy_any(&s, &d, true)?;
        }
        Ok(())
    } else {
        let mut r = File::open(src)?;
        let mut w = aux_rt::fs::create(dst, 0o644)?;
        aux_rt::copy(&mut r, &mut w)?;
        Ok(())
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
