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
// libthyla-rs uplift — typed Rust modules (U-2a..U-2-test).
// =============================================================================
//
// Per docs/UTOPIA-SHELL-DESIGN.md §15, libthyla-rs is being uplifted
// from "raw SVC wrappers + constants" into an idiomatic Rust API
// covering every Thylacine kernel surface. Each U-2X sub-chunk lands
// one module; existing callers continue to use the bare wrappers
// + T_* constants below for backwards compatibility during the
// transition.
//
// U-2a: err + handle.
// U-2b: alloc.

pub mod err;
pub mod handle;
pub mod alloc;

// =============================================================================
// Syscall numbers — MUST mirror kernel/include/thylacine/syscall.h.
// =============================================================================

pub const T_SYS_EXITS: u64           = 0;
pub const T_SYS_PUTS: u64            = 1;
pub const T_SYS_MMIO_CREATE: u64     = 2;
pub const T_SYS_IRQ_CREATE: u64      = 3;
pub const T_SYS_IRQ_WAIT: u64        = 4;
pub const T_SYS_MMIO_MAP: u64        = 5;
pub const T_SYS_DMA_CREATE: u64      = 6;
pub const T_SYS_DMA_MAP: u64         = 7;
// P5-fd-* family — byte I/O surface used by corvus's Spoor server loop.
pub const T_SYS_PIPE: u64            = 8;
pub const T_SYS_READ: u64            = 9;
pub const T_SYS_WRITE: u64           = 10;
pub const T_SYS_CLOSE: u64           = 11;
// P5-corvus-syscalls (kernel side at 0db0dcf/d10d4ee). v1.0 hardening
// syscalls used by /sbin/corvus startup.
pub const T_SYS_MLOCKALL: u64        = 16;
pub const T_SYS_SET_DUMPABLE: u64    = 17;
pub const T_SYS_SET_TRACEABLE: u64   = 18;
pub const T_SYS_EXPLICIT_BZERO: u64  = 19;
pub const T_SYS_GETRANDOM: u64       = 20;
pub const T_SYS_SPAWN_FULL: u64      = 25;
// P5-corvus-srv-impl-a/b: /srv mechanism — kernel-owned 9P transport.
pub const T_SYS_POST_SERVICE: u64    = 26;
pub const T_SYS_SRV_ACCEPT: u64      = 27;
pub const T_SYS_SRV_PEER: u64        = 28;
// P5-poll-a: the multi-fd wait/wake primitive. Backs corvus's main loop
// (a single thread serving N /srv/corvus connections) and the future
// musl `poll(2)` shim. ABI matches Linux event values for shim triviality.
pub const T_SYS_POLL: u64            = 29;
pub const T_SYS_SRV_CONNECT: u64     = 30;
// P5-corvus-srv-impl-b3a: spawn_full + perm_flags (atomic kernel stamp
// of PROC_FLAG_* bits on the spawned child, gated on caller console-attach).
pub const T_SYS_SPAWN_WITH_PERMS: u64 = 31;
// P5-hostowner-b-b: register / redeem a pending cap grant via the `cap`
// device (CORVUS-DESIGN.md §5.5.1). Bridges the kernel cap-grant table
// to userspace until a generic t_open lands.
pub const T_SYS_CAP_GRANT: u64        = 32;
pub const T_SYS_CAP_USE: u64          = 33;

// P6-pouch-mem: anonymous-memory grow/shrink, the Tier-1 native memory
// primitive (ARCHITECTURE.md §6.5). Backs t::alloc's #[global_allocator]
// in libthyla-rs (U-2b) and every libt-equivalent userspace allocator.
pub const T_SYS_BURROW_ATTACH: u64    = 37;
pub const T_SYS_BURROW_DETACH: u64    = 38;

// SYS_SPAWN_WITH_PERMS perm_flags — must mirror SPAWN_PERM_* in
// kernel/include/thylacine/syscall.h.
pub const T_SPAWN_PERM_MAY_POST_SERVICE: u64 = 1 << 0;

// poll event bits — MUST mirror POLL* in kernel/include/thylacine/poll.h.
// Linux values; the future musl shim is a no-op.
pub const T_POLLIN: i16   = 0x001;
pub const T_POLLOUT: i16  = 0x004;
pub const T_POLLERR: i16  = 0x008;
pub const T_POLLHUP: i16  = 0x010;
pub const T_POLLNVAL: i16 = 0x020;

// =============================================================================
// Caps — MUST mirror CAP_* bits in kernel/include/thylacine/caps.h.
// =============================================================================

pub const T_CAP_HW_CREATE: u64       = 1 << 0;
pub const T_CAP_LOCK_PAGES: u64      = 1 << 1;
pub const T_CAP_CSPRNG_READ: u64     = 1 << 2;
pub const T_CAP_HOSTOWNER: u64       = 1 << 3;   // elevation-only; not in CAP_ALL
pub const T_CAP_GRANT_HOSTOWNER: u64 = 1 << 4;   // fork-grantable; joey → corvus

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

// The right-bit subset that the kernel will accept on a fresh hw-handle
// (MMIO / IRQ / DMA). I-5 forbids transfer; the kernel rejects
// RIGHT_TRANSFER on hw handles at create time. Drivers should pass a
// subset of this when constructing hw handles.
pub const T_RIGHT_HW_ALLOWED: u32 =
    T_RIGHT_READ | T_RIGHT_WRITE | T_RIGHT_MAP | T_RIGHT_SIGNAL;

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
// Returns the (collapsed) pending count consumed (always ≥ 1 on the
// success path — `kobj_irq_wait` blocks on a rendez until pending_count
// strictly exceeds zero, then atomically reads-and-clears under the
// rendez lock); -1 on validation failure (bad handle, missing
// RIGHT_SIGNAL).
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

// t_dma_create — create a KObj_DMA handle backed by `size` bytes of
// kernel-allocated contiguous pinned memory. Requires CAP_HW_CREATE in
// proc->caps. Size must be > 0 and ≤ 1 MiB at v1.0; kernel rounds up to
// the next page boundary. Returns a non-negative handle index on
// success, -1 on cap missing / size out of range / OOM.
//
// The DMA buffer's PA is chosen by the kernel and is stable for the
// handle's lifetime. Use t_dma_map to install it in your address space
// and obtain the PA for use in device-visible descriptors.
#[inline(always)]
pub unsafe fn t_dma_create(size: u64, rights: u32) -> i64 {
    let mut x0: i64 = size as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") rights as u64,
        in("x8") T_SYS_DMA_CREATE,
        options(nostack)
    );
    x0
}

// t_dma_map — install user-VA mappings for a KObj_DMA handle and return
// the underlying PA. `vaddr` must be page-aligned (4 KiB); `prot` must be
// non-zero, only R/W bits set (EXEC rejected per W^X), no W-without-R.
//
// Returns the buffer's PA on success (always non-negative since PA fits
// in 40 bits at v1.0), -1 on validation failure. Driver embeds the PA
// into device-visible descriptors (VirtIO virtqueue rings, etc.).
//
// Safety: handle must be valid + held by the caller.
#[inline(always)]
pub unsafe fn t_dma_map(handle: i64, vaddr: u64, prot: u32) -> i64 {
    let mut x0: i64 = handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") vaddr,
        in("x2") prot as u64,
        in("x8") T_SYS_DMA_MAP,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-fd-* family — byte I/O surface.
// =============================================================================
//
// Mirror of usr/lib/libt/include/thyla/syscall.h::t_pipe / t_read /
// t_write / t_close. The kernel-side handlers landed at P5-fd-pipe
// (0f66f5c/65293f5), P5-fd-rw (54d900b/a134782), P5-fd-syscalls
// (5fd72f6/3948bd4).

// t_pipe — create a connected Spoor pair. On success, returns the
// (rd_fd, wr_fd) tuple. On failure (table full / OOM), returns
// (-1, 0).
//
// Both fds are KOBJ_SPOOR handles installed in the caller's table.
// The 4 KiB ring buffer is shared between them; spoor_clunk on either
// side propagates EOF to the other.
#[inline(always)]
pub unsafe fn t_pipe() -> (i64, i64) {
    let mut x0: i64;
    let mut x1: i64;
    asm!(
        "svc #0",
        out("x0") x0,
        out("x1") x1,
        in("x8") T_SYS_PIPE,
        options(nostack)
    );
    (x0, x1)
}

// t_read — read up to `len` bytes from `fd` into `buf`. Returns:
//   > 0  : bytes actually read (may be < len; caller loops for full reads)
//   = 0  : EOF (peer closed write side)
//   < 0  : error (-1 on invalid fd / bad buf / fault)
//
// Per-call cap is 4 KiB (kernel-side SYS_RW_MAX); userspace loops for
// larger transfers.
//
// Safety: caller must ensure `buf` points to at least `len` writable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_read(fd: i64, buf: *mut u8, len: usize) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") buf as u64,
        in("x2") len as u64,
        in("x8") T_SYS_READ,
        options(nostack)
    );
    x0
}

// t_write — write up to `len` bytes from `buf` to `fd`. Returns:
//   > 0  : bytes actually written (may be < len)
//   = 0  : peer's read side closed (EOF on write)
//   < 0  : error (-1)
//
// Safety: caller must ensure `buf` points to at least `len` readable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_write(fd: i64, buf: *const u8, len: usize) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") buf as u64,
        in("x2") len as u64,
        in("x8") T_SYS_WRITE,
        options(nostack)
    );
    x0
}

// pollfd — userspace ABI of SYS_POLL. The kernel struct pollfd is
// `{ i32 fd; i16 events; i16 revents }` (8 bytes). `#[repr(C)]` pins
// the layout; the static asserts in <thylacine/poll.h> pin the kernel
// side. fd is an i32 handle index; events is a bitmask of T_POLL*;
// revents is kernel-filled.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug)]
pub struct TPollFd {
    pub fd: i32,
    pub events: i16,
    pub revents: i16,
}

// t_poll — block until at least one of `fds` (a slice of `TPollFd`)
// becomes ready, or `timeout_ms` elapses. `timeout_ms`:
//   < 0 → block indefinitely (poll(-1));
//   = 0 → return immediately after the first scan (non-blocking probe);
//   > 0 → block for at most `timeout_ms` milliseconds.
//
// Returns the number of pollfds with `revents != 0` (≥ 0), or -1 on
// error (nfds == 0 or > 64, or fds points outside user-VA).
//
// `nfds` is bounded by the kernel at PROC_HANDLE_MAX (64). The kernel
// writes `revents` back to each pollfd in `fds` in place.
//
// Safety: `fds` must point to `nfds` writable `TPollFd` records in
// valid user-VA memory.
#[inline(always)]
pub unsafe fn t_poll(fds: *mut TPollFd, nfds: usize, timeout_ms: i32) -> i64 {
    let mut x0: i64 = fds as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") nfds as u64,
        in("x2") timeout_ms as i64 as u64,
        in("x8") T_SYS_POLL,
        options(nostack)
    );
    x0
}

// t_close — release the handle at `fd`. For KOBJ_SPOOR handles the
// kernel's release path routes through spoor_clunk (atomic refcount
// drop; sets pipe EOF + wakes the other side per P5-pipe-blocking).
// Returns 0 on success, -1 on invalid fd.
#[inline(always)]
pub unsafe fn t_close(fd: i64) -> i64 {
    let mut x0: i64 = fd;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_CLOSE,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-corvus-syscalls — v1.0 hardening syscalls.
// =============================================================================
//
// Implemented at P5-corvus-syscalls (commit 0db0dcf/d10d4ee). These five
// wrappers are the Rust mirror of the C-side stubs in
// usr/lib/libt/include/thyla/syscall.h. Used by /sbin/corvus (and any
// future security-sensitive daemon) at startup.

// t_mlockall — pin all currently-mapped and future-mapped pages so they
// cannot be evicted to swap or any future paging tier. `flags` is
// reserved at v1.0 (must be 0). Sets PROC_FLAG_MLOCKED on the Proc.
//
// Requires CAP_LOCK_PAGES in proc->caps. Returns 0 on success, -1 on
// missing cap or non-zero flags. Once set, the flag is permanent for
// the Proc's lifetime — there's no t_munlockall at v1.0.
#[inline(always)]
pub unsafe fn t_mlockall(flags: u64) -> i64 {
    let mut x0: i64 = flags as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_MLOCKALL,
        options(nostack)
    );
    x0
}

// t_set_dumpable — control core-dump permission for the calling Proc.
// One-way to 0: t_set_dumpable(0) sets PROC_FLAG_NODUMP (permanent);
// t_set_dumpable(1) on a Proc that already has the flag is REFUSED
// (kernel returns -1). Returns 0 on first successful set-to-0; -1 on
// any other input or attempted re-enable.
//
// Core dumps don't exist at v1.0 — the flag is forward-compat
// scaffolding. When core dumps land, the kernel-side dump path must
// check this flag and refuse to dump a Proc with NODUMP set.
#[inline(always)]
pub unsafe fn t_set_dumpable(dumpable: u64) -> i64 {
    let mut x0: i64 = dumpable as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SET_DUMPABLE,
        options(nostack)
    );
    x0
}

// t_set_traceable — control debug-Spoor attach permission. Same
// one-way-to-0 semantics as t_set_dumpable. Sets PROC_FLAG_NOTRACE.
//
// Debug Spoors don't exist at v1.0 — the flag is forward-compat
// scaffolding. When debug-Spoor attach lands, the kernel-side attach
// path must check this flag and refuse to attach to a Proc with
// NOTRACE set.
#[inline(always)]
pub unsafe fn t_set_traceable(traceable: u64) -> i64 {
    let mut x0: i64 = traceable as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SET_TRACEABLE,
        options(nostack)
    );
    x0
}

// t_explicit_bzero — compiler-barrier'd memset to zero of `len` bytes at
// `buf`. The kernel performs a per-byte uaccess_store_u8 loop which the
// optimizer cannot elide. Returns 0 on success, -1 on validation
// failure (buf in kernel-VA, len > SYS_RW_MAX, mid-stream fault).
//
// Use this for in-RAM secrets immediately after they're consumed —
// passphrase buffers, derived KEKs, unwrapped DEKs. Without it, the
// compiler's dead-store elimination can remove a plain `*buf = 0; *buf
// = 0; ...` loop entirely.
//
// Safety: caller must ensure `buf` points to at least `len` writable
// bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_explicit_bzero(buf: *mut u8, len: usize) -> i64 {
    let mut x0: i64 = buf as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") len as u64,
        in("x8") T_SYS_EXPLICIT_BZERO,
        options(nostack)
    );
    x0
}

// t_getrandom — read `len` random bytes into `buf` from the kernel
// CSPRNG. `flags` is reserved at v1.0 (must be 0). Caller must hold
// CAP_CSPRNG_READ. Per-call cap is SYS_RW_MAX (4 KiB) at v1.0.
//
// Returns `len` on success, -1 on cap missing / non-zero flags /
// oversized len / mid-stream uaccess fault. The kernel CSPRNG is
// seeded from ARM RNDR at boot; if RNDR is unavailable or returns
// failure, getrandom returns -1 (caller must NOT proceed with
// guessable entropy).
//
// On mid-stream uaccess failure (kernel/syscall.c::sys_getrandom_handler
// per R15-d F237 close), the kernel best-effort zeros the partial range
// before returning -1, so a caller seeing -1 should still NOT trust the
// buffer's prior contents.
//
// Safety: caller must ensure `buf` points to at least `len` writable
// bytes in valid user-VA memory + hold CAP_CSPRNG_READ.
#[inline(always)]
pub unsafe fn t_getrandom(buf: *mut u8, len: usize, flags: u64) -> i64 {
    let mut x0: i64 = buf as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") len as u64,
        in("x2") flags,
        in("x8") T_SYS_GETRANDOM,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-corvus-srv-impl-b3b — /srv service syscall wrappers.
// =============================================================================
//
// Used by corvus (server side) and joey (client side) to consume the
// kernel-owned /srv transport landed at P5-corvus-srv-impl-a/-b2.

// TSrvPeerInfo — SYS_SRV_PEER's writeback buffer. Layout pinned by
// kernel-side struct srv_peer_info (kernel/include/thylacine/syscall.h)
// to a fixed 24-byte ABI record. Repr-C + the kernel's static asserts
// pin both sides.
#[repr(C)]
#[derive(Copy, Clone, Default, Debug)]
pub struct TSrvPeerInfo {
    pub stripes: u64,
    pub caps: u64,
    pub console: u32,
    pub alive: u32,
}

// t_post_service — register the calling Proc as the server for service
// `name`. Requires PROC_FLAG_MAY_POST_SERVICE (the joey-stamped post
// gate; see t_spawn_with_perms below). Returns a non-negative KObj_Srv
// listener handle on success, -1 on:
//   - gate missing (no PROC_FLAG_MAY_POST_SERVICE)
//   - bad name (zero len, >SRV_NAME_MAX, NUL bytes)
//   - registry full / name already posted by a live Proc / OOM
//
// On success, future client SYS_SRV_CONNECT(name, ...) calls flow
// through this listener and become t_srv_accept-able. The listener
// handle is NON-TRANSFERABLE (KObj_Srv partition of handles.tla).
//
// Safety: `name` must point to `name_len` readable bytes in valid
// user-VA memory.
#[inline(always)]
pub unsafe fn t_post_service(name: *const u8, name_len: usize) -> i64 {
    let mut x0: i64 = name as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name_len as u64,
        in("x8") T_SYS_POST_SERVICE,
        options(nostack)
    );
    x0
}

// t_srv_accept — block until a client connects, return the server-side
// endpoint as a KObj_Spoor handle (byte I/O — plain t_read/t_write). The
// handshake (Tversion + Tattach + Twalk + Tlopen) was driven by the
// kernel before the client's SYS_SRV_CONNECT returned, so the very first
// t_read on the returned handle observes a fully-attached client.
//
// Each accepted connection has its own kernel-stamped peer identity;
// fetch it via t_srv_peer on the returned handle. The Spoor returned is
// transferable (KObj_Spoor) — corvus can hand it to a worker thread once
// concurrency lands at v1.x. At v1.0 corvus serves all conns single-
// threaded via t_poll.
//
// Returns the connection Spoor fd (>=0) on success, -1 on:
//   - service_handle is not a KObj_Srv listener
//   - listener torn down (poster exited / unposted)
//   - blocked accept woken by signal (v1.x — at v1.0 there are none)
#[inline(always)]
pub unsafe fn t_srv_accept(service_handle: i64) -> i64 {
    let mut x0: i64 = service_handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_SRV_ACCEPT,
        options(nostack)
    );
    x0
}

// t_srv_peer — read the kernel-stamped peer identity of a /srv
// connection into `out`. The fields are:
//   - stripes : the peer Proc's identity tag (immutable by-value capture
//               at mint; 0 means "the peer never had stripes assigned" —
//               unreachable at v1.0).
//   - caps    : the peer's LIVE capability set (looked up under the
//               proc table lock at call time). If the peer Proc has
//               exited/been reaped (`alive == 0`), caps is 0.
//   - console : 1 iff the peer was console-attached at mint (immutable).
//   - alive   : 1 iff an ALIVE Proc still carries this stripes value at
//               call time, else 0. Use this to decide whether to honor
//               caps; an alive==0 peer's caps is fail-closed.
//
// Refuses any caller whose stripes differ from the connection's poster
// (the gate prevents a non-server Proc from reading peer identity off a
// connection it just SYS_SRV_ACCEPT'd onto someone else's listener —
// which can't happen at v1.0 but is structurally pinned).
//
// Returns 0 on success, -1 on bad handle / wrong kind / SrvConn
// gate violation / bad user-VA.
//
// Safety: `out` must point to a writable TSrvPeerInfo (24 bytes) in
// valid user-VA memory.
#[inline(always)]
pub unsafe fn t_srv_peer(connection_handle: i64, out: *mut TSrvPeerInfo) -> i64 {
    let mut x0: i64 = connection_handle;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") out as u64,
        in("x8") T_SYS_SRV_PEER,
        options(nostack)
    );
    x0
}

// t_srv_connect — open a client-side connection to `name` and walk one
// path component `path` (typically `"ctl"` for the Stratum-style 9P
// /srv pattern). The kernel mints a SrvConn, drives the full handshake
// (Tversion + Tattach + Twalk(path) + Tlopen) on the caller's behalf,
// and returns a KObj_Srv client handle. Subsequent t_read / t_write on
// the returned handle translate to Tread / Twrite at the open fid.
//
// Per-Proc cap: at v1.0 a Proc may hold at most ONE concurrent /srv
// client connection (kernel/srvconn.c SRV_CONN_PER_PROC_MAX=1). A
// second t_srv_connect from a Proc that already holds one returns -1
// until t_close releases the prior handle.
//
// Returns the client KObj_Srv fd (>=0) on success, -1 on:
//   - service unposted (no LIVE service named `name`)
//   - cap exhausted (this Proc already holds a connection)
//   - handshake refused by the server (bad Rversion/Rattach/Rwalk/Rlopen)
//   - kernel-side OOM / handle table full
//
// Safety: `name` / `path` must point to `name_len` / `path_len`
// readable bytes in valid user-VA memory.
#[inline(always)]
pub unsafe fn t_srv_connect(name: *const u8, name_len: usize,
                            path: *const u8, path_len: usize) -> i64 {
    let mut x0: i64 = name as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name_len as u64,
        in("x2") path as u64,
        in("x3") path_len as u64,
        in("x8") T_SYS_SRV_CONNECT,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-corvus-srv-impl-b3a — t_spawn_with_perms.
// =============================================================================
//
// SYS_SPAWN_WITH_PERMS extends SYS_SPAWN_FULL with a sixth argument
// `perm_flags` carrying T_SPAWN_PERM_* bits. The kernel stamps each set
// bit as the corresponding PROC_FLAG_* on the spawned child atomically
// inside the spawn thunk (BEFORE the child's exec_setup), so the child's
// very first user-mode instruction observes the final proc_flags.
//
// Any nonzero perm_flags bit requires the calling Proc to be console-
// attached. Designed for joey (the v1.0 console anchor): joey grants
// /sbin/corvus T_SPAWN_PERM_MAY_POST_SERVICE so corvus may call
// SYS_POST_SERVICE("corvus"). Bits are NOT propagated by rfork (the
// design's "kernel-stamped" gate, not a cap).
//
// Returns the child PID (>0) on success, -1 on:
//   - perm_flags has unknown bits
//   - any perm_flags bit set AND caller is not console-attached
//   - any condition that t_spawn_full would return -1 on
//
// Safety: `name` must point to `name_len` readable bytes; `fds` (if
// fd_count > 0) must point to `fd_count` u32 entries; all in valid
// user-VA.
#[inline(always)]
pub unsafe fn t_spawn_with_perms(name: *const u8, name_len: usize,
                                 fds: *const u32, fd_count: usize,
                                 cap_mask: u64, perm_flags: u64) -> i64 {
    let mut x0: i64 = name as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") name_len as u64,
        in("x2") fds as u64,
        in("x3") fd_count as u64,
        in("x4") cap_mask,
        in("x5") perm_flags,
        in("x8") T_SYS_SPAWN_WITH_PERMS,
        options(nostack)
    );
    x0
}

// =============================================================================
// P5-hostowner-b-b — t_cap_grant / t_cap_use.
// =============================================================================
//
// Userspace bridges to the kernel `cap` elevation device
// (CORVUS-DESIGN.md §5.5.1). The Dev write op (devcap_write) is the
// eventual production path via a future namespace-aware open; at v1.0
// we lack t_open, so the two writers reach the cores directly.
//
// t_cap_grant — register a pending grant for `target_stripes`. Caller
// must hold CAP_GRANT_HOSTOWNER. Returns 0 on success, -1 on gate fail /
// bad args / table full. At v1.0 only CAP_HOSTOWNER is grantable.
#[inline(always)]
pub unsafe fn t_cap_grant(cap_mask: u64, target_stripes: u64) -> i64 {
    let mut x0: i64 = cap_mask as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") target_stripes,
        in("x8") T_SYS_CAP_GRANT,
        options(nostack)
    );
    x0
}

// t_cap_use — redeem a pending grant for the caller's own stripes.
// Caller must hold PROC_FLAG_CONSOLE_ATTACHED and have a non-expired
// pending grant with matching cap_mask. On success, caller's caps gains
// `cap_mask`; the grant is consumed (one-shot). Returns 0 on success,
// -1 on gate fail / no pending grant / mismatch.
#[inline(always)]
pub unsafe fn t_cap_use(cap_mask: u64) -> i64 {
    let mut x0: i64 = cap_mask as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_CAP_USE,
        options(nostack)
    );
    x0
}

// t_burrow_attach — request `length` bytes of anonymous, demand-zero,
// read-write memory and have the kernel install it in the calling
// Proc's address space. Returns the page-aligned base user-VA on
// success (always >= EXEC_USER_BURROW_BASE = 0x0000_0001_0000_0000),
// -1 on:
//   - length == 0 or length > BURROW_ATTACH_MAX (= 256 MiB)
//   - no free gap of round_up(length) in the burrow window
//   - burrow_create_anon / burrow_map OOM
//
// The kernel rounds `length` up to a page (4 KiB) and chooses the
// virtual address (first-fit in EXEC_USER_BURROW_BASE..TOP). Pages
// install on demand via the user-fault path.
//
// Tier-1 native memory primitive (ARCHITECTURE.md §6.5). Backs
// libthyla-rs's t::alloc global heap (U-2b).
#[inline(always)]
pub unsafe fn t_burrow_attach(length: u64) -> i64 {
    let mut x0: i64 = length as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x8") T_SYS_BURROW_ATTACH,
        options(nostack)
    );
    x0
}

// t_burrow_detach — release a region previously attached by
// t_burrow_attach. The (vaddr, page-rounded length) must match an
// installed VMA exactly — no partial detach at v1.0 (mirrors the
// kernel-side burrow_unmap constraint). Returns 0 on success, -1 on:
//   - length == 0 or length > BURROW_ATTACH_MAX
//   - vaddr not page-aligned
//   - no VMA matches [vaddr, vaddr + round_up(length)) exactly
//
// `length` may be the original request OR any value that page-rounds
// to the same span; the kernel matches on the rounded range.
#[inline(always)]
pub unsafe fn t_burrow_detach(vaddr: u64, length: u64) -> i64 {
    let mut x0: i64 = vaddr as i64;
    asm!(
        "svc #0",
        inlateout("x0") x0,
        in("x1") length,
        in("x8") T_SYS_BURROW_DETACH,
        options(nostack)
    );
    x0
}

// =============================================================================
// VIRTIO memory-ordering helpers.
// =============================================================================
//
// virtio_rmb — read-side barrier for the driver's view of the used ring.
// VIRTIO 1.2 §2.7.13.2 mandates the driver execute a barrier of this
// shape between observing `used.idx` advance and reading `used.ring[k]`
// or the data buffer the descriptor pointed at. Without it, an
// out-of-order ARM core may speculatively issue the data-buffer reads
// before the used.idx load, returning stale (pre-advance) bytes.
//
// `dmb ishld` is the LoadLoad barrier scoped to the Inner Shareable
// domain — the minimum sufficient for guest-CPU-vs-guest-CPU ordering of
// Normal-WB memory backing the virtqueue. Matches what Linux's
// `virtio_rmb()` compiles to on AArch64.
//
// Today on QEMU TCG (in-order execution) this is a no-op in practice,
// but emitting it preserves correctness on real ARM cores (the v1.0
// deployment target). The cost is one barrier instruction per used.idx
// read.
#[inline(always)]
pub fn virtio_rmb() {
    unsafe { asm!("dmb ishld", options(nostack, preserves_flags)) }
}

// =============================================================================
// _start — entry point.
// =============================================================================
//
// The kernel's userland_enter delivers control here with sp pointing at
// the System V startup frame's argc word (EXEC_USER_STACK_TOP -
// EXEC_INIT_STACK_SIZE; P6-pouch-kernel-auxv). libthyla-rs's _start does
// not consume the frame — it calls rs_main() directly. Defined in asm
// for full control over BTI marker + branch sequence; mirrors
// usr/lib/libt/src/start.S (the C side's _start).
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
