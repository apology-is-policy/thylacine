// mkdir [-p] DIR... -- create directories.
//
// -p creates parents as needed and does not error if a directory already
// exists. Uses aux-rt::fs::mkdir (t_walk_create + DMDIR). Absolute paths
// only (no cwd at v1.0).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::err::Error;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut parents = false;
    while let Some(a) = args.get_str(idx) {
        match a {
            "-p" => {
                parents = true;
                idx += 1;
            }
            "--" => {
                idx += 1;
                break;
            }
            _ if a.starts_with('-') && a != "-" => {
                aux_rt::eprintln!("mkdir: unknown option {}", a);
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
                aux_rt::eprintln!("mkdir: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        let r = if parents {
            mkdir_p(path)
        } else {
            aux_rt::fs::mkdir(path, 0o755)
        };
        if let Err(e) = r {
            aux_rt::eprintln!("mkdir: {}: {}", path, e);
            status = 1;
        }
    }
    if !had {
        aux_rt::eprintln!("mkdir: missing operand");
        return 1;
    }
    status
}

// Create each ancestor of an absolute path, then the leaf, ignoring "already
// exists" at every step.
fn mkdir_p(path: &str) -> aux_rt::Result<()> {
    let bytes = path.as_bytes();
    let mut i = 1; // skip the leading '/'
    while i <= bytes.len() {
        if i > 1 && (i == bytes.len() || bytes[i] == b'/') {
            let prefix = &path[..i];
            match aux_rt::fs::mkdir(prefix, 0o755) {
                Ok(()) | Err(Error::Exists) => {}
                Err(e) => return Err(e),
            }
        }
        i += 1;
    }
    Ok(())
}
