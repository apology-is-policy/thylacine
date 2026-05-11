// /hello-rs — first Rust userspace binary (P4-Ia2; refactored at P4-Ic4
// to depend on libthyla-rs).
//
// Mirror of usr/hello/hello.c (the C-side first userspace binary) but
// in Rust no_std + no_main, demonstrating the Rust path through the
// same build → cpio → devramfs → exec_setup → userland_enter pipeline.
//
// At P4-Ic4 the SVC wrappers + _start + #[panic_handler] live in
// libthyla-rs (sibling of usr/lib/libt). hello-rs is now a minimal
// program body: just `rs_main`.
//
// Layout (per usr/scripts/aarch64-userspace.ld + .cargo/config.toml's
// rustflags):
//   - aarch64-unknown-none target
//   - static, no PIE, code-model=small
//   - rust-lld with W^X linker script
//   - bti + pac hardening enabled
//   - panic = "abort" → libthyla-rs's #[panic_handler] → t_exits(1)

#![no_std]
#![no_main]

use libthyla_rs::t_putstr;

// rs_main — the Rust-level program body. Called from libthyla-rs's
// _start. Return value flows through x0 → SYS_EXITS status. Returning
// 0 ⇒ kernel records exits("ok"); non-zero ⇒ exits("fail"). The
// signature is `extern "C"` so the AArch64 PCS rules apply (x0 = return).
#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("hello from /hello-rs (Rust no_std, built via cargo)\n");
    0
}
