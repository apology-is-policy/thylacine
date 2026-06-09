// mkdir [-p] DIR... -- create directories.
//
// -p: create missing parent directories as needed, and do not error if a
// directory already exists. Uses libthyla-rs::fs::create_dir (SYS_WALK_CREATE
// + DMDIR). Absolute paths only at v1.0 (no cwd until LS-4).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::{Error, Result};
use libthyla_rs::{eprintln, fs};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut parents = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'p' => parents = true,
                    _ => {
                        eprintln!("mkdir: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut had = false;
    let mut status = 0;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("mkdir: invalid UTF-8 in operand");
                status = 1;
                continue;
            }
        };
        let r = if parents { mkdir_p(path) } else { fs::create_dir(path) };
        if let Err(e) = r {
            eprintln!("mkdir: {}: {}", path, e);
            status = 1;
        }
    }
    if !had {
        eprintln!("mkdir: missing operand");
        return 1;
    }
    status
}

// `mkdir -p`: create each ancestor then the leaf, treating an already-present
// component as success. Only meaningful for an absolute path (v1.0 has no cwd,
// LS-4); a relative path falls through to create_dir's InvalidArgument.
fn mkdir_p(path: &str) -> Result<()> {
    if !path.starts_with('/') {
        return fs::create_dir(path);
    }
    let mut acc = String::new();
    for comp in path.split('/').filter(|s| !s.is_empty()) {
        acc.push('/');
        acc.push_str(comp);
        match fs::create_dir(acc.as_str()) {
            Ok(()) | Err(Error::Exists) => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}
