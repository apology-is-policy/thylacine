// rmdir DIR... -- remove empty directories (aux-rt::fs::remove_dir ->
// t_unlink with the REMOVEDIR flag). A non-empty directory errors.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                aux_rt::eprintln!("rmdir: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if let Err(e) = aux_rt::fs::remove_dir(path) {
            aux_rt::eprintln!("rmdir: {}: {}", path, e);
            status = 1;
        }
    }
    if !had {
        aux_rt::eprintln!("rmdir: missing operand");
        return 1;
    }
    status
}
