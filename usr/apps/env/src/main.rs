// env [NAME=VALUE...] [COMMAND...] -- print or modify the environment.
//
// DEGENERATE at v1.0 (DOC-GAP G15): Thylacine exposes NO environment to a
// native program. envp is inherited-only at the kernel (SYS_SPAWN_FULL_ARGV
// has a reserved `_pad_envp` but no envp pass-through), and there is no
// getenv/environ accessor. So:
//   - `env` with no operands prints nothing (the environment IS empty).
//   - `env` with operands (set vars and/or run a command) is unsupported.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    if args.operands().next().is_some() {
        aux_rt::eprintln!(
            "env: setting variables / running a command is unsupported at v1.0 (no envp surface)"
        );
        return 125; // env's documented exit status for a usage/setup failure
    }
    // Empty environment: print nothing, succeed.
    0
}
