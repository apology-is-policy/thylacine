// unmount MOUNTPOINT... -- remove namespace mount entries (Plan 9 unmount).
//
// Removes the mount(s) at MOUNTPOINT from the calling Proc's territory
// (territory::unmount). Absolute paths only. Like bind, this affects only
// this Proc's namespace (ARCH I-1).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;
use libthyla_rs::territory;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let mut status = 0;
    let mut had = false;
    for op in args.operands() {
        had = true;
        let point = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                aux_rt::eprintln!("unmount: invalid UTF-8 path");
                status = 1;
                continue;
            }
        };
        if let Err(e) = territory::unmount(point) {
            aux_rt::eprintln!("unmount: {}: {}", point, e);
            status = 1;
        }
    }
    if !had {
        aux_rt::eprintln!("unmount: missing operand");
        return 1;
    }
    status
}
