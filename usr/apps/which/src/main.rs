// which NAME... -- locate a command.
//
// DEGENERATE at v1.0 (DOC-GAP G15/G07): there is no PATH environment
// variable (no envp surface) and no cwd, so there is nothing to search. A
// NAME containing '/' is treated as an explicit absolute path and probed
// for existence; a bare NAME cannot be resolved (reported not-found, exit
// 1, like `which` on a miss).

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
        let name = match core::str::from_utf8(op) {
            Ok(n) => n,
            Err(_) => {
                status = 1;
                continue;
            }
        };
        if name.contains('/') {
            // Explicit path: report it if it exists.
            if libthyla_rs::fs::exists(name) {
                aux_rt::out(name.as_bytes());
                aux_rt::out(b"\n");
            } else {
                status = 1;
            }
        } else {
            // No PATH to search (G15): cannot resolve a bare command name.
            status = 1;
        }
    }
    if !had {
        return 1;
    }
    status
}
