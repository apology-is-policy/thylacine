// stat FILE... -- display file metadata (via libthyla-rs::fs::metadata ->
// SYS_FSTAT). Output is a compact, readable block per file.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::fs::Metadata;

aux_rt::main!(run);

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
                aux_rt::eprintln!("stat: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        match libthyla_rs::fs::metadata(path) {
            Ok(m) => {
                aux_rt::print!("  File: {}\n", path);
                aux_rt::print!(
                    "  Size: {}\tBlocks: {}\tIO Block: {}\t{}\n",
                    m.len(),
                    m.blocks(),
                    m.blksize(),
                    type_word(&m)
                );
                aux_rt::print!(
                    "  Mode: ({:04o})\tLinks: {}\tUid: ({})\tGid: ({})\n",
                    m.permissions() & 0o7777,
                    m.nlink(),
                    m.uid(),
                    m.gid()
                );
                aux_rt::print!(
                    "Access: {}\tModify: {}\tChange: {}\n",
                    m.atime_sec(),
                    m.mtime_sec(),
                    m.ctime_sec()
                );
            }
            Err(e) => {
                aux_rt::eprintln!("stat: {}: {}", path, e);
                status = 1;
            }
        }
    }
    if !had {
        aux_rt::eprintln!("stat: missing operand");
        return 1;
    }
    status
}
