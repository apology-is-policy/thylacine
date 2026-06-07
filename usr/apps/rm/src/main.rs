// rm [-rRf] FILE... -- remove files (and, with -r, directory trees).
//
// -r/-R recurse into directories; -f ignores nonexistent files and
// suppresses error messages. Uses aux-rt::fs (metadata + read_dir +
// remove_file/remove_dir). Recursion is depth-first.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::err::Error;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut recursive = false;
    let mut force = false;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'r' | 'R' => recursive = true,
                    'f' => force = true,
                    _ => {
                        aux_rt::eprintln!("rm: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut status = 0;
    let mut had = false;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                if !force {
                    aux_rt::eprintln!("rm: invalid UTF-8 path");
                    status = 1;
                }
                continue;
            }
        };
        if !rm_path(path, recursive, force) {
            status = 1;
        }
    }
    if !had && !force {
        aux_rt::eprintln!("rm: missing operand");
        return 1;
    }
    status
}

// Returns true on success. Prints its own errors (unless force).
fn rm_path(path: &str, recursive: bool, force: bool) -> bool {
    match libthyla_rs::fs::metadata(path) {
        Ok(m) if m.is_dir() => {
            if !recursive {
                if !force {
                    aux_rt::eprintln!("rm: cannot remove '{}': Is a directory", path);
                }
                return false;
            }
            let mut ok = true;
            match aux_rt::fs::read_dir(path) {
                Ok(entries) => {
                    for e in entries {
                        if e.name == "." || e.name == ".." {
                            continue;
                        }
                        let child = join(path, &e.name);
                        if !rm_path(&child, true, force) {
                            ok = false;
                        }
                    }
                }
                Err(e) => {
                    if !force {
                        aux_rt::eprintln!("rm: {}: {}", path, e);
                    }
                    ok = false;
                }
            }
            if ok {
                if let Err(e) = aux_rt::fs::remove_dir(path) {
                    if !force {
                        aux_rt::eprintln!("rm: {}: {}", path, e);
                    }
                    ok = false;
                }
            }
            ok
        }
        Ok(_) => match aux_rt::fs::remove_file(path) {
            Ok(()) => true,
            Err(e) => {
                if !force {
                    aux_rt::eprintln!("rm: {}: {}", path, e);
                }
                false
            }
        },
        Err(e) => {
            if force && e == Error::NotFound {
                return true; // rm -f ignores a missing target
            }
            if !force {
                aux_rt::eprintln!("rm: {}: {}", path, e);
            }
            false
        }
    }
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}
