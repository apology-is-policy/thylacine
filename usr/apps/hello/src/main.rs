// usr/apps/hello -- A0 bootstrap: the minimal native libthyla-rs app.
//
// Purpose: prove the auxiliary cargo workspace + the inherited
// aarch64-unknown-none target + the W^X linker script all compose into a
// linkable native ELF, before any real app depends on the toolchain. This
// is the auxiliary-track twin of usr/hello-rs (the main-tree first binary)
// and is deliberately identical in shape so a divergence in the build path
// shows up here first.
//
// Entry contract (docs/reference/38-userspace.md): libthyla-rs supplies
// _start (global_asm) which does `bl rs_main`; rs_main's i64 return flows
// through x0 -> SYS_EXITS (0 => exits("ok"), non-zero => exits("fail")).
// The binary must be #![no_std] #![no_main] and opt into the global
// allocator.
//
// Output: t_putstr writes to the kernel diagnostic UART (SYS_PUTS), NOT to
// fd 1 -- the same path usr/hello-rs uses. Real coreutils stdout (fd 1) is
// a separate surface mapped at A1 (see DOC-GAP-REPORT.md).

#![no_std]
#![no_main]

// Every native Rust binary opts in to ThylaAlloc as its global allocator;
// libthyla-rs links the alloc crate at its root and the symbol resolves
// here (docs/reference/38-userspace.md; usr/hello-rs convention).
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::t_putstr;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("hello from usr/apps/hello (native libthyla-rs, aux track)\n");
    0
}
