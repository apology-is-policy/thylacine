// libthyla-rs — Thylacine userspace runtime (Rust side).
//
// Mirror of usr/lib/libt/ on the Rust side. Provides:
//   - Syscall numbers (T_SYS_*) — kernel/include/thylacine/syscall.h.
//   - Right bits (T_RIGHT_*)    — kernel/include/thylacine/handle.h.
//   - Prot bits (T_PROT_*)      — kernel/include/thylacine/vma.h.
//   - SVC wrappers              — t_exits, t_puts, t_putstr, t_mmio_create,
//                                 t_mmio_map, t_irq_create, t_irq_wait.
//   - _start                    — global_asm; ENTRY(_start) in
//                                 usr/scripts/aarch64-userspace.ld keeps it
//                                 alive across the rlib boundary so the
//                                 binary doesn't need to redeclare it.
//   - #[panic_handler]          — default impl tail-calls t_exits(1). A
//                                 binary that wants a richer handler depends
//                                 on libthyla-rs with `default-features =
//                                 false` and provides its own (Phase 5+
//                                 hook).
//
// Binaries link libthyla-rs and define `#[no_mangle] pub extern "C" fn
// rs_main() -> i64` as their entry point. _start invokes rs_main and
// tail-calls SYS_EXITS with the return value.
//
// Extracted at P4-Ic4 from usr/hello-rs/src/main.rs (where these lived
// inline at P4-Ia2) to unblock P4-Ic5 (the virtio-blk driver crate).

#![no_std]

use core::arch::{asm, global_asm};
use core::panic::PanicInfo;

// =============================================================================
// Syscall numbers — MUST mirror kernel/include/thylacine/syscall.h.
// =============================================================================

pub const T_SYS_EXITS: u64        = 0;
pub const T_SYS_PUTS: u64         = 1;
pub const T_SYS_MMIO_CREATE: u64  = 2;
pub const T_SYS_IRQ_CREATE: u64   = 3;
pub const T_SYS_IRQ_WAIT: u64     = 4;
pub const T_SYS_MMIO_MAP: u64     = 5;

// =============================================================================
// Rights — MUST mirror RIGHT_* bits in kernel/include/thylacine/handle.h.
// =============================================================================

pub const T_RIGHT_READ: u32     = 1 << 0;
pub const T_RIGHT_WRITE: u32    = 1 << 1;
pub const T_RIGHT_MAP: u32      = 1 << 2;
pub const T_RIGHT_TRANSFER: u32 = 1 << 3;
pub const T_RIGHT_DMA: u32      = 1 << 4;
pub const T_RIGHT_SIGNAL: u32   = 1 << 5;
// MUST mirror the kernel-side RIGHT_ALL = 0x3f (six bits). A constant
// included here so consuming crates can assert their requested rights
// stay within the bound at compile time (Phase 5+ rights expansion).
pub const T_RIGHT_ALL: u32      = 0x3f;

// =============================================================================
// Prot bits — MUST mirror VMA_PROT_* in kernel/include/thylacine/vma.h.
// =============================================================================

pub const T_PROT_READ: u32  = 1 << 0;
pub const T_PROT_WRITE: u32 = 1 << 1;
pub const T_PROT_EXEC: u32  = 1 << 2;

// =============================================================================
// SVC wrappers.
// =============================================================================

// t_exits — terminate the calling process with `status`. Never returns.
// status==0 ⇒ kernel exits("ok"); non-zero ⇒ exits("fail").
#[inline(always)]
pub unsafe fn t_exits(status: i64) -> ! {
    asm!(
        "svc #0",
        in("x0") status,
        in("x8") T_SYS_EXITS,
        options(noreturn, nostack)
    );
}

// t_puts — write `len` bytes from `buf` to the kernel diagnostic UART.
// Returns `len` on success, -1 on validation failure (NULL buf,
// oversized len, fault on user-VA copy).
//
// Safety: caller must ensure `buf` points to at least `len` readable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_puts(buf: *const u8, len: usize) -> i64 {
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
pub fn t_putstr(s: &str) -> i64 {
    unsafe { t_puts(s.as_ptr(), s.len()) }
}

// t_mmio_create — create a KObj_MMIO handle for the PA range
// [pa, pa+size). Requires CAP_HW_CREATE in proc->caps. Returns a
// non-negative handle index on success, -1 on cap missing / overlap /
// alignment / IPS-bound / kernel-reserved-range rejection.
//
// Safety: caller must hold the capability + the PA range must be a
// real device range (not RAM owned by the kernel). The kernel-side
// kobj_mmio_create enforces every check; this wrapper just marshals
// args.
#[inline(always)]
pub unsafe fn t_mmio_create(pa: u64, size: u64, rights: u32) -> i64 {
    let mut x0: i64 = pa as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") size,
        in("x2") rights as u64,
        in("x8") T_SYS_MMIO_CREATE,
        options(nostack)
    );
    x0
}

// t_mmio_map — install user-VA mappings for a KObj_MMIO handle.
// `vaddr` must be page-aligned (4 KiB); `prot` must be non-zero, only
// R/W bits set, no EXEC (kernel rejects EXEC+device-memory per R10
// F157), and W-without-R is rejected (R10 F155 — AArch64 has no W-only
// AP encoding). Returns 0 on success, -1 on validation failure.
//
// Safety: handle must be valid + held by the caller.
#[inline(always)]
pub unsafe fn t_mmio_map(handle: i64, vaddr: u64, prot: u32) -> i64 {
    let mut x0: i64 = handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") vaddr,
        in("x2") prot as u64,
        in("x8") T_SYS_MMIO_MAP,
        options(nostack)
    );
    x0
}

// t_irq_create — create a KObj_IRQ handle for the given INTID.
// Requires CAP_HW_CREATE + the INTID must be SPI (32..1019; SGI/PPI
// are kernel-reserved per R9 F142 + F145). Returns a non-negative
// handle index on success, -1 on cap missing / out-of-range /
// already-claimed.
#[inline(always)]
pub unsafe fn t_irq_create(intid: u32, rights: u32) -> i64 {
    let mut x0: i64 = intid as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") rights as u64,
        in("x8") T_SYS_IRQ_CREATE,
        options(nostack)
    );
    x0
}

// t_irq_wait — block until the KObj_IRQ has fired one or more times.
// Returns the (collapsed) pending count consumed; 0 on spurious wake;
// -1 on validation failure (bad handle, missing RIGHT_SIGNAL).
//
// Edge-triggered: multiple fires while the waiter is blocked collapse
// to a single counter increment per actual GIC dispatch, but the
// returned value reflects the count seen at wake time.
#[inline(always)]
pub unsafe fn t_irq_wait(handle: i64) -> i64 {
    let mut x0: i64 = handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_IRQ_WAIT,
        options(nostack)
    );
    x0
}

// =============================================================================
// _start — entry point.
// =============================================================================
//
// The kernel's userland_enter delivers control here with
// sp=EXEC_USER_STACK_TOP. Defined in asm for full control over BTI
// marker + branch sequence; mirrors usr/lib/libt/src/start.S (the C
// side's _start).
//
// Flow:
//   bti c                  — BTI landing pad for indirect entry.
//   bl rs_main             — x0 := rs_main()
//   mov x8, T_SYS_EXITS    — syscall number
//   svc #0                 — never returns
//   wfe + b 1b             — defensive: SYS_EXITS doesn't return, but
//                            if the kernel were to deliver us back
//                            (impossible at v1.0), park forever.
//
// ENTRY(_start) in usr/scripts/aarch64-userspace.ld keeps _start as a
// liveness root, so the linker pulls this symbol from libthyla-rs.rlib
// even though no Rust code references it directly.
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

// =============================================================================
// Panic handler.
// =============================================================================
//
// Required by no_std. v1.0 P4-Ic4 response: SYS_EXITS(1). Phase 5+ can
// add a richer panic path (panic message → t_puts) via a Cargo feature
// flag once the runtime crate matures.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { t_exits(1) }
}
