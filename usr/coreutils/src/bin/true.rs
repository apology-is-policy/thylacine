// true -- do nothing, successfully. Always exits 0. Ignores all arguments
// (POSIX: `true` takes no options and always succeeds).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    0
}
