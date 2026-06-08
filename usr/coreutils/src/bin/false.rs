// false -- do nothing, unsuccessfully. Always exits 1. Ignores all
// arguments (POSIX: `false` takes no options and always fails).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    1
}
