// /hello-rs — first Rust userspace binary (P4-Ia2).
//
// Mirror of usr/hello/hello.c (the C-side first userspace binary) but
// in Rust no_std + no_main, demonstrating the Rust path through the
// same build → cpio → devramfs → exec_setup → userland_enter pipeline.
//
// At v1.0 P4-Ia2 the SVC wrappers live inline here. Subsequent sub-chunks
// (likely P4-Ic when the second Rust binary needs them) will extract a
// `libthyla-rs` crate exposing equivalents to `<thyla/syscall.h>`.
//
// Layout (per usr/scripts/aarch64-userspace.ld + .cargo/config.toml's
// rustflags):
//   - aarch64-unknown-none target
//   - static, no PIE, code-model=small
//   - rust-lld with W^X linker script
//   - bti + pac hardening enabled
//   - panic = "abort" → __rust_panic propagates straight to t_exits(1)

#![no_std]
#![no_main]

use core::arch::{asm, global_asm};
use core::panic::PanicInfo;

// Syscall numbers — MUST mirror kernel/include/thylacine/syscall.h.
const T_SYS_EXITS: u64 = 0;
const T_SYS_PUTS: u64 = 1;

// t_exits — terminate the calling process with `status`. Never returns.
// status==0 ⇒ kernel exits("ok"); non-zero ⇒ exits("fail").
//
// Safety: invokes a kernel SVC; the kernel side guarantees no return
// to this stack frame. Marked noreturn so the compiler knows code
// after this is unreachable.
#[inline(always)]
unsafe fn t_exits(status: i64) -> ! {
    asm!(
        "svc #0",
        in("x0") status,
        in("x8") T_SYS_EXITS,
        options(noreturn, nostack)
    );
}

// t_puts — write `len` bytes from `buf` to the kernel diagnostic UART.
// Returns `len` on success, -1 on validation failure (NULL buf, oversized
// len, fault on user-VA copy).
//
// Safety: caller must ensure buf points to at least len readable bytes
// in valid user-VA memory.
#[inline(always)]
unsafe fn t_puts(buf: *const u8, len: usize) -> i64 {
    let mut x0: i64 = buf as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") len as u64,
        in("x8") T_SYS_PUTS,
        options(nostack)
    );
    x0
}

// t_putstr — convenience: write a `&str` via t_puts. Safe wrapper
// because `&str` carries length and points to valid memory.
#[inline]
fn t_putstr(s: &str) -> i64 {
    unsafe { t_puts(s.as_ptr(), s.len()) }
}

// _start — entry point. The kernel's userland_enter delivers control
// here with sp=EXEC_USER_STACK_TOP. Defined in asm for full control
// over BTI marker + branch sequence; mirrors usr/lib/libt/src/start.S
// (the C side's _start) but lives in this crate because Rust's
// no_main forbids using an external _start from a .a library.
//
// Flow: bl rs_main → on return, SYS_EXITS with x0 = main's return value.
global_asm!(
    ".section .text._start, \"ax\"",
    ".globl _start",
    ".type _start, %function",
    "_start:",
    "    bti     c",                  // BTI landing pad for indirect entry
    "    bl      rs_main",            // x0 := rs_main()
    "    mov     x8, #0",             // T_SYS_EXITS
    "    svc     #0",                 // never returns
    "1:  wfe",                        // defensive; SYS_EXITS doesn't return
    "    b       1b",
    ".size _start, .-_start",
);

// rs_main — the Rust-level program body. Called from _start.
//
// Return value flows through x0 → SYS_EXITS status. Returning 0 ⇒
// kernel records exits("ok"); non-zero ⇒ exits("fail"). The signature
// is `extern "C"` so the AArch64 PCS rules apply (x0 = return).
#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("hello from /hello-rs (Rust no_std, built via cargo)\n");
    0
}

// Panic handler — required by no_std. v1.0 P4-Ia2 response: SYS_EXITS(1).
// Phase 5+ richer panic output (panic message → UART via a yet-to-land
// t_putstr_panic helper) when the Rust runtime crate is factored out.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { t_exits(1) }
}
