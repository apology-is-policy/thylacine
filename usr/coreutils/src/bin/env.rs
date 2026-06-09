// env [NAME=VALUE...] [COMMAND...] -- print or modify the environment.
//
// DEGENERATE at v1.0 (DOC-GAP G15): Thylacine exposes NO environment to a
// native program. envp is inherited-only at the kernel (SYS_SPAWN_FULL_ARGV
// has a reserved envp slot but no pass-through), and there is no
// getenv/environ accessor. So:
//   - `env` with no operands prints nothing (the environment IS empty).
//   - `env` with operands (set vars and/or run a command) is unsupported.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn run(args: Args) -> i64 {
    if args.operands().next().is_some() {
        eprintln!(
            "env: setting variables / running a command is unsupported at v1.0 (no envp surface)"
        );
        return 125; // env's documented exit status for a usage/setup failure
    }
    // Empty environment: print nothing, succeed.
    0
}
