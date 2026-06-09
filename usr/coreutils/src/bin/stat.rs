// stat FILE... -- display file metadata (via fs::metadata -> SYS_FSTAT).
// Output is a compact, readable block per file.
//
// Adopted (LS-3a) from usr/apps/stat (aux/userspace-apps), rewritten off the
// aux-rt workaround onto the libthyla-rs surface (env::args + the print /
// eprintln macros). atime/mtime/ctime are 0 at v1.0 (most Devs don't track
// timestamps; see fs::Metadata).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{self, Metadata};
use libthyla_rs::{eprintln, print};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn type_word(m: &Metadata) -> &'static str {
    if m.is_dir() {
        "directory"
    } else if m.is_char_device() {
        "character special file"
    } else if m.is_symlink() {
        "symbolic link"
    } else {
        "regular file"
    }
}

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("stat: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        match fs::metadata(path) {
            Ok(m) => {
                print!("  File: {}\n", path);
                print!(
                    "  Size: {}\tBlocks: {}\tIO Block: {}\t{}\n",
                    m.len(),
                    m.blocks(),
                    m.blksize(),
                    type_word(&m)
                );
                print!(
                    "  Mode: ({:04o})\tLinks: {}\tUid: ({})\tGid: ({})\n",
                    m.permissions() & 0o7777,
                    m.nlink(),
                    m.uid(),
                    m.gid()
                );
                print!(
                    "Access: {}\tModify: {}\tChange: {}\n",
                    m.atime_sec(),
                    m.mtime_sec(),
                    m.ctime_sec()
                );
            }
            Err(e) => {
                eprintln!("stat: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        eprintln!("stat: missing operand");
        return 1;
    }
    status
}
