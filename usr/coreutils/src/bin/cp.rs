// cp [-rR] SRC... DST -- copy files (and, with -r, directory trees).
//
// Forms: `cp SRC DST` (DST a file or existing dir) and `cp SRC... DIR`.
// -r / -R: recursively copy a directory. Recursion skips "." / ".." (fs::read_dir
// yields them). A file copy is open(SRC) + create(DST,trunc) + io::copy.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::{self, File, Path};
use libthyla_rs::{eprintln, io};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut recursive = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'r' | 'R' => recursive = true,
                    _ => {
                        eprintln!("cp: invalid option -- '{}'", ch);
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
                eprintln!("cp: invalid UTF-8 in operand");
                return 1;
            }
        }
    }
    if ops.len() < 2 {
        eprintln!("cp: missing file operand");
        return 1;
    }

    let dest = ops[ops.len() - 1];
    let sources = &ops[..ops.len() - 1];
    let dest_is_dir = fs::is_dir(dest);
    if sources.len() > 1 && !dest_is_dir {
        eprintln!("cp: target '{}' is not a directory", dest);
        return 1;
    }

    let mut status = 0;
    for src in sources {
        if fs::is_dir(src) && !recursive {
            eprintln!("cp: -r not specified; omitting directory '{}'", src);
            status = 1;
            continue;
        }
        let target = if dest_is_dir {
            join(dest, base(src))
        } else {
            String::from(dest)
        };
        if let Err(e) = cp_one(src, target.as_str()) {
            eprintln!("cp: {} -> {}: {}", src, target, e);
            status = 1;
        }
    }
    status
}

// Copy `src` to `dst`. A directory here always means recursive mode (the top
// level gated the dir-without-r case), so recurse into it.
fn cp_one(src: &str, dst: &str) -> Result<()> {
    match fs::metadata(src) {
        Ok(m) if m.is_dir() => {
            match fs::create_dir(dst) {
                Ok(()) | Err(Error::Exists) => {}
                Err(e) => return Err(e),
            }
            for ent in fs::read_dir(src)? {
                let e = ent?;
                let name = e.file_name();
                if name == "." || name == ".." {
                    continue;
                }
                cp_one(&join(src, name), &join(dst, name))?;
            }
            Ok(())
        }
        Ok(_) => copy_file(src, dst),
        Err(e) => Err(e),
    }
}

fn copy_file(src: &str, dst: &str) -> Result<()> {
    let mut r = File::open(src)?;
    let mut w = File::create(dst)?;
    io::copy(&mut r, &mut w)?;
    Ok(())
}

fn base(path: &str) -> &str {
    Path::new(path).file_name().unwrap_or(path)
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
