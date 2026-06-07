// ls [-la1] [DIR...] -- list directory contents (one name per line).
//
// -a includes dotfiles (and . / ..); -l is the long format (mode, links,
// uid, gid, size, name); -1 is one-per-line (the default; no column
// packing at v1). With no operand, lists "/" (no cwd at v1.0, G07). Uses
// aux-rt::fs::read_dir + libthyla-rs::fs::metadata (for -l).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::fs::Metadata;

aux_rt::main!(run);

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
                        aux_rt::eprintln!("ls: invalid option -- '{}'", ch);
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
            Err(_) => aux_rt::eprintln!("ls: invalid UTF-8 path"),
        }
    }
    if dirs.is_empty() {
        dirs.push("/");
    }

    let multi = dirs.len() > 1;
    let mut status = 0;
    let mut first = true;
    for dir in dirs {
        if multi {
            if !first {
                aux_rt::out(b"\n");
            }
            aux_rt::print!("{}:\n", dir);
        }
        first = false;
        if let Err(e) = list_dir(dir, all, long) {
            aux_rt::eprintln!("ls: {}: {}", dir, e);
            status = 1;
        }
    }
    status
}

fn list_dir(dir: &str, all: bool, long: bool) -> aux_rt::Result<()> {
    let mut entries = aux_rt::fs::read_dir(dir)?;
    if !all {
        entries.retain(|e| !e.name.starts_with('.'));
    }
    entries.sort_unstable_by(|a, b| a.name.cmp(&b.name));

    for e in &entries {
        if long {
            let full = join(dir, &e.name);
            match libthyla_rs::fs::metadata(&full) {
                Ok(m) => print_long(&m, &e.name),
                Err(_) => aux_rt::print!("??????????    ? {}\n", e.name),
            }
        } else {
            aux_rt::out(e.name.as_bytes());
            aux_rt::out(b"\n");
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
    aux_rt::print!(
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
