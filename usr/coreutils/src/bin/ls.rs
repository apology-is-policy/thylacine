// ls [-la1] [DIR...] -- list directory contents (one name per line).
//
// -a includes dotfiles (and . / ..); -l is the long format (mode, links,
// uid, gid, size, name); -1 is one-per-line (the default; no column
// packing at v1.0). With no operand, lists "/" (no cwd until LS-4, G07).
// Uses fs::read_dir (SYS_READDIR) + fs::metadata (SYS_FSTAT, for -l).
//
// Adopted (LS-3a) from usr/apps/ls (aux/userspace-apps), rewritten off the
// aux-rt workaround onto the libthyla-rs surface: aux_rt::fs::read_dir (a
// Vec) becomes libthyla_rs::fs::read_dir (a streaming ReadDir iterator), so
// names are collected before the filter + sort.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Result;
use libthyla_rs::fs::{self, Metadata};
use libthyla_rs::io;
use libthyla_rs::{eprintln, print};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut all = false;
    let mut long = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'a' => all = true,
                    'l' => long = true,
                    '1' => {}
                    _ => {
                        eprintln!("ls: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut dirs: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => dirs.push(p),
            Err(_) => eprintln!("ls: invalid UTF-8 path"),
        }
    }
    // No operand -> list the per-Proc cwd (LS-4), not the territory root. Held
    // in cwd_holder so its &str lives as long as `dirs` (RW-9 R4-F4).
    let cwd_holder = if dirs.is_empty() {
        Some(env::current_dir().unwrap_or_else(|_| String::from("/")))
    } else {
        None
    };
    if let Some(ref c) = cwd_holder {
        dirs.push(c.as_str());
    }

    let multi = dirs.len() > 1;
    let mut status = 0;
    let mut first = true;
    for dir in dirs {
        if multi {
            if !first {
                io::out(b"\n");
            }
            print!("{}:\n", dir);
        }
        first = false;
        if let Err(e) = list_dir(dir, all, long) {
            eprintln!("ls: {}: {}", dir, e);
            status = 1;
        }
    }
    status
}

fn list_dir(dir: &str, all: bool, long: bool) -> Result<()> {
    let mut names: Vec<String> = Vec::new();
    for ent in fs::read_dir(dir)? {
        let name = ent?.into_file_name();
        if !all && name.starts_with('.') {
            continue;
        }
        names.push(name);
    }
    names.sort_unstable();

    for name in &names {
        if long {
            let full = join(dir, name);
            match fs::metadata(&full) {
                Ok(m) => print_long(&m, name),
                Err(_) => print!("??????????    ? {}\n", name),
            }
        } else {
            io::out(name.as_bytes());
            io::out(b"\n");
        }
    }
    Ok(())
}

fn print_long(m: &Metadata, name: &str) {
    let mut perms = [b'-'; 10];
    perms[0] = if m.is_dir() {
        b'd'
    } else if m.is_symlink() {
        b'l'
    } else if m.is_char_device() {
        b'c'
    } else {
        b'-'
    };
    let p = m.permissions();
    let bits: [(u32, usize, u8); 9] = [
        (0o400, 1, b'r'),
        (0o200, 2, b'w'),
        (0o100, 3, b'x'),
        (0o040, 4, b'r'),
        (0o020, 5, b'w'),
        (0o010, 6, b'x'),
        (0o004, 7, b'r'),
        (0o002, 8, b'w'),
        (0o001, 9, b'x'),
    ];
    for (mask, i, ch) in bits {
        if p & mask != 0 {
            perms[i] = ch;
        }
    }
    let ps = core::str::from_utf8(&perms).unwrap_or("??????????");
    print!(
        "{} {:>3} {:>5} {:>5} {:>9} {}\n",
        ps,
        m.nlink(),
        m.uid(),
        m.gid(),
        m.len(),
        name
    );
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
