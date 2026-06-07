// bind [-abr] SOURCE MOUNTPOINT -- Plan 9 namespace bind.
//
// Makes the tree at SOURCE appear at MOUNTPOINT in the calling Proc's
// per-Proc namespace (territory). -r replace (default), -b before, -a after
// (union-directory ordering). SOURCE is opened as a directory handle (File
// impls AsFd) and handed to territory::mount.
//
// Both paths are ABSOLUTE (no cwd, G07). MOUNTPOINT must already exist as a
// walkable directory (Plan 9 M1). The change affects ONLY this Proc's
// namespace (ARCH I-1) -- a standalone `bind` invocation binds in its own
// namespace and exits, so it is mainly useful from a shell that stays alive
// (or a spawn that inherits the namespace).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::fs::File;
use libthyla_rs::territory::{self, MountFlags};

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut idx = 1;
    let mut flags = MountFlags::REPL;
    while let Some(a) = args.get_str(idx) {
        match a {
            "-r" => {
                flags = MountFlags::REPL;
                idx += 1;
            }
            "-b" => {
                flags = MountFlags::BEFORE;
                idx += 1;
            }
            "-a" => {
                flags = MountFlags::AFTER;
                idx += 1;
            }
            "--" => {
                idx += 1;
                break;
            }
            _ if a.starts_with('-') && a != "-" => {
                aux_rt::eprintln!("bind: unknown option {}", a);
                return 1;
            }
            _ => break,
        }
    }

    let (src, mnt) = match (args.get_str(idx), args.get_str(idx + 1)) {
        (Some(s), Some(m)) => (s, m),
        _ => {
            aux_rt::eprintln!("bind: usage: bind [-abr] SOURCE MOUNTPOINT");
            return 1;
        }
    };

    let handle = match File::open(src) {
        Ok(f) => f,
        Err(e) => {
            aux_rt::eprintln!("bind: {}: {}", src, e);
            return 1;
        }
    };
    if let Err(e) = territory::mount(&handle, mnt, flags) {
        aux_rt::eprintln!("bind: {} -> {}: {}", src, mnt, e);
        return 1;
    }
    0
}
