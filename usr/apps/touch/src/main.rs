// touch FILE... -- create empty files; for existing files, would update the
// mtime.
//
// PARTIAL at v1.0 (DOC-GAP G12): t_wstat exposes only mode/uid/gid -- there
// is NO atime/mtime setter. So touch can CREATE an absent file (empty) but
// cannot update the timestamp of an existing one; for an existing file it is
// a no-op. `-c` (no-create) is honored.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut no_create = false;
    while let Some(a) = args.get_str(idx) {
        match a {
            "-c" => {
                no_create = true;
                idx += 1;
            }
            "--" => {
                idx += 1;
                break;
            }
            _ if a.starts_with('-') && a != "-" => {
                aux_rt::eprintln!("touch: unknown option {}", a);
                return 1;
            }
            _ => break,
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
                aux_rt::eprintln!("touch: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if libthyla_rs::fs::exists(path) {
            // Existing file: an mtime bump is unsupported (G12) -> no-op.
            continue;
        }
        if no_create {
            continue;
        }
        match aux_rt::fs::create(path, 0o644) {
            Ok(_f) => { /* created empty; _f closes on drop */ }
            Err(e) => {
                aux_rt::eprintln!("touch: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        aux_rt::eprintln!("touch: missing operand");
        return 1;
    }
    status
}
