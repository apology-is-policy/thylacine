// pwd -- print the working directory.
//
// Prints the per-Proc current directory (LS-4 landed SYS_GETCWD/SYS_CHDIR and
// env::current_dir()). Before LS-4 there was no cwd concept, so this hardcoded
// the territory-root anchor "/"; the substrate moved and the binary was never
// revisited (RW-9 R4-F4 -- the LS-4-stale coreutil group).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    match libthyla_rs::env::current_dir() {
        Ok(cwd) => {
            libthyla_rs::io::out(cwd.as_bytes());
            libthyla_rs::io::out(b"\n");
            0
        }
        // cwd defaults to "/" (territory root); a getcwd failure still reports
        // the root anchor but with a nonzero status.
        Err(_) => {
            libthyla_rs::io::out(b"/\n");
            1
        }
    }
}
