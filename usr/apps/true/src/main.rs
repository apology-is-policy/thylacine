// true -- do nothing, successfully. Always exits 0. Ignores all arguments
// (POSIX: `true` takes no options and always succeeds).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

aux_rt::main!(run);

fn run(_args: aux_rt::Args) -> i64 {
    0
}
