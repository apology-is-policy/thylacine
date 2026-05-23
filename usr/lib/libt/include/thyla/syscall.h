// Thylacine userspace syscall wrappers (libt — Thylacine userspace runtime).
//
// At P4-Ia1 the userspace syscall surface is exactly two operations
// (matching kernel/include/thylacine/syscall.h):
//
//   t_exits(status)     → SYS_EXITS  (terminate; status==0 ⇒ "ok", else "fail")
//   t_puts(buf, len)    → SYS_PUTS   (write `len` bytes to UART)
//
// Phase 5+ extends the surface: open / close / read / write / mount / bind /
// rfork / wait / mmap / munmap / notify. Each new syscall appends an inline
// wrapper here; the dispatch number lives in the kernel header.
//
// Calling convention (matches kernel/include/thylacine/syscall.h §AArch64 ABI):
//   x8       = syscall number
//   x0..x5   = positional arguments
//   x0       = return value
//
// The kernel saves the full exception_context on SVC entry and restores it
// on eret, so userspace observes ALL GPRs preserved across a syscall except
// x0 (return value). Inline asm clobber list is therefore "memory" + "cc"
// only — not the full caller-saved set.
//
// Header-only by design: trivial wrappers don't justify an out-of-line .a
// path. _start (the program-entry stub) lives in libt.a because it isn't
// inlinable.

#ifndef THYLA_SYSCALL_H
#define THYLA_SYSCALL_H

#include <stddef.h>

// Syscall numbers. MUST mirror kernel/include/thylacine/syscall.h.
enum {
    T_SYS_EXITS       = 0,
    T_SYS_PUTS        = 1,
    T_SYS_MMIO_CREATE = 2,  // P4-Ib: create KObj_MMIO handle
    T_SYS_IRQ_CREATE  = 3,  // P4-Ib: create KObj_IRQ handle
    T_SYS_IRQ_WAIT    = 4,  // P4-Ib: block until IRQ fires
    T_SYS_MMIO_MAP    = 5,  // P4-Ic2: map KObj_MMIO into user-VA
    T_SYS_DMA_CREATE  = 6,  // P4-Ic5b1b: allocate KObj_DMA handle
    T_SYS_DMA_MAP     = 7,  // P4-Ic5b1b: map KObj_DMA into user-VA, returns PA
    T_SYS_PIPE        = 8,  // P5-fd-pipe: create connected pair; rd in x0, wr in x1
    T_SYS_READ        = 9,  // P5-fd-rw: read(fd, buf, len)
    T_SYS_WRITE       = 10, // P5-fd-rw: write(fd, buf, len)
    T_SYS_CLOSE       = 11, // P5-fd-syscalls: close(fd)
    T_SYS_DUP         = 12, // P5-fd-syscalls: dup(oldfd, new_rights) → newfd
    T_SYS_ATTACH_9P   = 13, // P5-attach-syscall: 9P client attach over Spoor pair
    T_SYS_MOUNT       = 14, // P5-mount-syscall: graft Spoor at target_path_id
    T_SYS_UNMOUNT     = 15, // P5-mount-syscall: remove mount entry at target_path_id
    T_SYS_MLOCKALL    = 16, // P5-corvus-syscalls: pin pages (CAP_LOCK_PAGES)
    T_SYS_SET_DUMPABLE = 17, // P5-corvus-syscalls: set NODUMP flag (one-way to 0)
    T_SYS_SET_TRACEABLE = 18, // P5-corvus-syscalls: set NOTRACE flag (one-way to 0)
    T_SYS_EXPLICIT_BZERO = 19, // P5-corvus-syscalls: barrier'd zeroize user-VA buf
    T_SYS_GETRANDOM   = 20, // P5-corvus-syscalls: read kernel CSPRNG (CAP_CSPRNG_READ)
    T_SYS_SPAWN       = 21, // P5-spawn-wait: rfork RFPROC + exec on a devramfs binary
    T_SYS_WAIT_PID    = 22, // P5-spawn-wait: reap one ZOMBIE child; write status if non-NULL
    T_SYS_SPAWN_WITH_FDS = 23, // P5-stratumd-stub-b: spawn + pre-install fds in child
    T_SYS_SPAWN_WITH_CAPS = 24, // P5-spawn-caps: spawn + grant cap-subset to child
    T_SYS_SPAWN_FULL  = 25, // P5-spawn-full: spawn + inherit fds + grant cap-subset
    // 26..29 — registered in <thylacine/syscall.h> (P5-corvus-srv + P5-poll);
    // libt has no C-side wrappers for them at v1.0 (corvus reaches them
    // via libthyla-rs; joey uses only the client-side wrapper below).
    T_SYS_SRV_CONNECT = 30,      // P5-corvus-srv-impl-b2: client-side /srv open
    T_SYS_SPAWN_WITH_PERMS = 31, // P5-corvus-srv-impl-b3a: spawn_full + perm_flags
    T_SYS_CAP_GRANT   = 32,      // P5-hostowner-b-b: register pending cap grant
    T_SYS_CAP_USE     = 33,      // P5-hostowner-b-b: redeem pending cap grant
    T_SYS_WALK_OPEN   = 34,      // P5-stratumd-stub-bringup-e1: walk-and-open one path component
    T_SYS_CHROOT      = 35,      // P5-stratumd-stub-bringup-e2: stamp territory root_spoor
    T_SYS_SET_TID_ADDRESS = 36,  // P6-pouch-kernel-auxv: return the calling thread's tid
    // 37 + 38 — SYS_BURROW_ATTACH / SYS_BURROW_DETACH (P6-pouch-mem-a).
    // No libt C-side wrappers yet — pouch's mmap/munmap reaches the
    // syscalls directly through the musl seam (0003-pouch-mman). Native
    // C consumers will land a t_burrow_attach / t_burrow_detach pair
    // when the first one materialises.
    T_SYS_TORPOR_WAIT = 39,      // P6-pouch-wait-addr: wait on a user-VA word
    T_SYS_TORPOR_WAKE = 40,      // P6-pouch-wait-addr: wake waiters on a user-VA word
    T_SYS_THREAD_SPAWN = 41,     // P6-pouch-threads: spawn an EL0 Thread in the caller's Proc
    T_SYS_THREAD_EXIT  = 42,     // P6-pouch-threads: exit the calling Thread; never returns
};

// Torpor error codes — match kernel's TORPOR_ERR_* (Linux/musl-numeric
// so the pouch syscall seam will decode them as -errno when the pouch
// pthread layer lands at sub-chunk 9). Native callers can branch on
// these directly; the value-mismatch fast path returns 0 (TORPOR_OK).
#define T_TORPOR_OK             0
#define T_TORPOR_ERR_EINVAL    (-22)
#define T_TORPOR_ERR_EFAULT    (-14)
#define T_TORPOR_ERR_ETIMEDOUT (-110)

// t_torpor_wait — wait until *addr_va may have changed and a matching
// t_torpor_wake arrives, OR the timeout elapses. Returns 0 on success
// (woken OR value mismatch), or one of the T_TORPOR_ERR_* errnos.
// `timeout_us < 0` blocks indefinitely. The 4-byte word at addr_va
// must be aligned and within the user VA bound.
__attribute__((always_inline))
static inline long t_torpor_wait(unsigned int *addr_va, unsigned int expected,
                                 long timeout_us) {
    register long x0 __asm__("x0") = (long)(unsigned long)addr_va;
    register long x1 __asm__("x1") = (long)expected;
    register long x2 __asm__("x2") = timeout_us;
    register long x8 __asm__("x8") = T_SYS_TORPOR_WAIT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_torpor_wake — wake up to `count` threads waiting on `*addr_va`.
// Returns the number of waiters actually woken (>= 0), or -EINVAL on
// bad args. `count == 0` is a no-op (returns 0); UINT32_MAX is the
// "wake all" pattern.
//
// F12 (P3 audit): `count` is `unsigned int` here so the wrapper cannot
// pass a u64-valued count to the SVC. The kernel SVC handler narrows
// the raw arg to u32 (`(u32)count_raw` in `sys_torpor_wake_handler`),
// so a Rust- or asm-side caller passing a u64 > UINT32_MAX would have
// its high bits silently discarded. UINT32_MAX already means "wake
// all"; values above it are semantically the same intent, so the
// narrowing is not a soundness concern, only a slightly-lossy ABI.
__attribute__((always_inline))
static inline long t_torpor_wake(unsigned int *addr_va, unsigned int count) {
    register long x0 __asm__("x0") = (long)(unsigned long)addr_va;
    register long x1 __asm__("x1") = (long)count;
    register long x8 __asm__("x8") = T_SYS_TORPOR_WAKE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_walk_open's FROM_ROOT sentinel — pass as spoor_fd to walk from the
// caller's pivoted territory root_spoor instead of a handle.
// P5-stratumd-stub-bringup-e2. Matches SYS_WALK_OPEN_FROM_ROOT.
#define T_WALK_OPEN_FROM_ROOT  ((long)(-1))

// SYS_WALK_OPEN omode bits — must mirror SYS_WALK_OPEN_OMODE_VALID in
// kernel/include/thylacine/syscall.h. Plan-9 modes: OREAD=0, OWRITE=1,
// ORDWR=2, OEXEC=3 in the low 2 bits, optional OTRUNC=0x10 modifier.
#define T_OREAD    0u
#define T_OWRITE   1u
#define T_ORDWR    2u
#define T_OEXEC    3u
#define T_OTRUNC   0x10u

// Maximum single-component name length for t_walk_open (matches the
// kernel cap; passing longer returns -1 from the syscall).
#define T_WALK_OPEN_NAME_MAX  64u

// SYS_SPAWN_WITH_PERMS perm_flags — must mirror SPAWN_PERM_* in
// kernel/include/thylacine/syscall.h. SPAWN_PERM_MAY_POST_SERVICE stamps
// PROC_FLAG_MAY_POST_SERVICE on the spawned child so it may call
// SYS_POST_SERVICE; granting it requires the parent to be console-
// attached.
#define T_SPAWN_PERM_MAY_POST_SERVICE  (1u << 0)

// SYS_GETRANDOM flags (mirror kernel/include/thylacine/syscall.h).
#define T_GRND_NONBLOCK   1u

// CAP_* bits — MUST mirror kernel/include/thylacine/caps.h. Used as
// `cap_mask` arg to t_spawn_with_caps / t_spawn_full / t_cap_grant /
// t_cap_use.
#define T_CAP_HW_CREATE       (1UL << 0)
#define T_CAP_LOCK_PAGES      (1UL << 1)
#define T_CAP_CSPRNG_READ     (1UL << 2)
#define T_CAP_HOSTOWNER       (1UL << 3)   // elevation-only; not in CAP_ALL
#define T_CAP_GRANT_HOSTOWNER (1UL << 4)   // fork-grantable; joey → corvus

// Maximum binary name length for t_spawn (mirror SYS_SPAWN_NAME_MAX).
#define T_SPAWN_NAME_MAX  64u

// Maximum inherited-fd count for t_spawn_with_fds (mirror SYS_SPAWN_MAX_FDS).
#define T_SPAWN_MAX_FDS   16u

// Mount flags — mirror kernel/include/thylacine/territory.h (Plan 9
// MREPL / MBEFORE / MAFTER / MCREATE). At v1.0 only MREPL has
// distinguished semantics (replace existing entry at the same target);
// MBEFORE / MAFTER / MCREATE are stored for future union-mount work.
#define T_MREPL    0x0001u
#define T_MBEFORE  0x0002u
#define T_MAFTER   0x0004u
#define T_MCREATE  0x0008u

// VMA prot bits — MUST mirror kernel/include/thylacine/vma.h's
// VMA_PROT_* values. Used as the 3rd argument to t_mmio_map.
#define T_PROT_READ       (1u << 0)
#define T_PROT_WRITE      (1u << 1)
#define T_PROT_EXEC       (1u << 2)

// Rights bits. MUST mirror kernel/include/thylacine/handle.h.
#define T_RIGHT_READ      (1u << 0)
#define T_RIGHT_WRITE     (1u << 1)
#define T_RIGHT_MAP       (1u << 2)
#define T_RIGHT_TRANSFER  (1u << 3)
#define T_RIGHT_DMA       (1u << 4)
#define T_RIGHT_SIGNAL    (1u << 5)

// t_exits — terminate the calling process with `status`. Never returns.
// status==0 maps to kernel exits("ok"); non-zero to exits("fail").
__attribute__((noreturn, always_inline))
static inline void t_exits(long status) {
    register long x0 __asm__("x0") = status;
    register long x8 __asm__("x8") = T_SYS_EXITS;
    __asm__ volatile (
        "svc #0"
        :: "r"(x0), "r"(x8)
        : "memory", "cc"
    );
    __builtin_unreachable();
}

// t_puts — write `len` bytes from `buf` to the kernel's diagnostic UART.
// Returns `len` on success, -1 on validation failure (NULL buf, oversized
// len, fault on user-VA copy).
__attribute__((always_inline))
static inline long t_puts(const char *buf, size_t len) {
    register long x0 __asm__("x0") = (long)(unsigned long)buf;
    register long x1 __asm__("x1") = (long)len;
    register long x8 __asm__("x8") = T_SYS_PUTS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_putstr — convenience wrapper: write a NUL-terminated string via t_puts.
// Computes strlen inline (no libc dependency).
__attribute__((always_inline))
static inline long t_putstr(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return t_puts(s, n);
}

// t_mmio_create — allocate a KObj_MMIO handle claiming the physical
// memory range [pa, pa+size). Returns a non-negative handle index on
// success, -1 on:
//   - EPERM (caller lacks T_RIGHT_HW_CREATE capability)
//   - EINVAL (pa or size not page-aligned, size == 0, rights invalid)
//   - EBUSY (PA range overlaps an already-claimed range)
//   - EOOM / handle-table-full
//
// Pinned by specs/handles.tla::HwResourceExclusive + ::HwHandleImpliesCap.
__attribute__((always_inline))
static inline long t_mmio_create(unsigned long pa, unsigned long size,
                                 unsigned long rights) {
    register long x0 __asm__("x0") = (long)pa;
    register long x1 __asm__("x1") = (long)size;
    register long x2 __asm__("x2") = (long)rights;
    register long x8 __asm__("x8") = T_SYS_MMIO_CREATE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_irq_create — allocate a KObj_IRQ handle subscribing to GIC `intid`.
// Returns a non-negative handle index on success, -1 on EPERM / EINVAL /
// EBUSY / OOM. Pinned by HwResourceExclusive + HwHandleImpliesCap.
__attribute__((always_inline))
static inline long t_irq_create(unsigned long intid, unsigned long rights) {
    register long x0 __asm__("x0") = (long)intid;
    register long x1 __asm__("x1") = (long)rights;
    register long x8 __asm__("x8") = T_SYS_IRQ_CREATE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_irq_wait — block until at least one IRQ has fired on the subscription
// represented by handle `h`. Returns the collapsed-fire count (>=1), or
// -1 on bad handle / wrong kind / missing T_RIGHT_SIGNAL.
__attribute__((always_inline))
static inline long t_irq_wait(long h) {
    register long x0 __asm__("x0") = h;
    register long x8 __asm__("x8") = T_SYS_IRQ_WAIT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_mmio_map — install a user-VA mapping for a KObj_MMIO handle. The
// PTEs are installed lazily on first access (demand-paging). Returns 0
// on success, -1 on:
//   - cap-missing (defense)
//   - bad handle (range, wrong kind, missing T_RIGHT_MAP)
//   - prot exceeds rights / EXEC requested / prot == 0
//   - vaddr unaligned / overflow / overlaps existing VMA
//   - OOM
//
// Pinned by specs/handles.tla::HwHandleImpliesCap (cap held) +
// specs/burrow.tla::NoUseAfterFree (Burrow lifecycle bound to VMA).
__attribute__((always_inline))
static inline long t_mmio_map(long h, unsigned long vaddr, unsigned long prot) {
    register long x0 __asm__("x0") = h;
    register long x1 __asm__("x1") = (long)vaddr;
    register long x2 __asm__("x2") = (long)prot;
    register long x8 __asm__("x8") = T_SYS_MMIO_MAP;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_dma_create — allocate a KObj_DMA handle backed by `size` bytes of
// kernel-allocated contiguous pinned memory. Requires T_RIGHT_HW_CREATE
// capability. Size must be > 0 and ≤ 1 MiB at v1.0; kernel page-aligns
// up. Returns a non-negative handle index on success, -1 on EPERM /
// EINVAL (bad size) / EOOM. The buffer's PA is chosen by the kernel
// and stable for the handle's lifetime.
//
// Pinned by specs/handles.tla::HwHandleImpliesCap +
// HwResourceExclusive (via buddy's per-alloc partitioning).
__attribute__((always_inline))
static inline long t_dma_create(unsigned long size, unsigned long rights) {
    register long x0 __asm__("x0") = (long)size;
    register long x1 __asm__("x1") = (long)rights;
    register long x8 __asm__("x8") = T_SYS_DMA_CREATE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_attach_9p — wrap a Spoor pair (tx, rx) in a 9P client + drive the
// Tversion + Tattach handshake; return a KOBJ_SPOOR fd for the 9P
// tree's root. For duplex transports (Unix socket, vsock — Phase 5+),
// pass the same fd for both tx_fd and rx_fd. For half-duplex pipes,
// pass the matching write-end + read-end.
//
// aname is a server-side path or capability string (up to 256 bytes
// at v1.0). n_uname is 0 for no-auth attach at v1.0; Phase 5+
// authentication backends will use it as the per-user identifier.
//
// Returns the new fd (>=0) on success, -1 on:
//   - invalid tx_fd / rx_fd or missing R/W rights
//   - aname out of user-VA bound / aname_len > 256
//   - server-side Rlerror on Tversion or Tattach
//   - kmalloc OOM / handle table full
__attribute__((always_inline))
static inline long t_attach_9p(long tx_fd, long rx_fd,
                               const char *aname, size_t aname_len,
                               unsigned long n_uname) {
    register long x0 __asm__("x0") = tx_fd;
    register long x1 __asm__("x1") = rx_fd;
    register long x2 __asm__("x2") = (long)(unsigned long)aname;
    register long x3 __asm__("x3") = (long)aname_len;
    register long x4 __asm__("x4") = (long)n_uname;
    register long x8 __asm__("x8") = T_SYS_ATTACH_9P;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_mount — graft the Spoor at `source_spoor_fd` at `target_path_id`
// in the caller's Territory mount table. Plan 9 `mount` semantics:
// the mount entry holds its OWN refcount on the source Spoor (per
// ARCH §9.6.6), so the caller can `t_close(source_spoor_fd)` after a
// successful mount; the mount table keeps the Spoor alive until
// `t_unmount` or Territory destruction.
//
// `target_path_id` is a u32 abstract path token at v1.0 — the same
// numeric ID used by bind/unbind. String-path resolution lands with
// the fd-syscall walk subsystem in a later chunk.
//
// `flags` is T_MREPL / T_MBEFORE / T_MAFTER / T_MCREATE (bit-or'd).
//
// Returns 0 on success, -1 on:
//   - invalid source_spoor_fd (not KOBJ_SPOOR or out-of-range)
//   - source handle missing T_RIGHT_READ
//   - flags has bits outside the valid set
//   - Territory mount table full (8 entries at v1.0)
__attribute__((always_inline))
static inline long t_mount(long source_spoor_fd, unsigned long target_path_id,
                           unsigned long flags) {
    register long x0 __asm__("x0") = source_spoor_fd;
    register long x1 __asm__("x1") = (long)target_path_id;
    register long x2 __asm__("x2") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_MOUNT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_unmount — remove the FIRST mount entry at `target_path_id` from
// the caller's Territory mount table; drop the source's per-entry
// refcount. (Union mounts with multiple entries at the same target
// require multiple t_unmount calls — Phase 5+ once walk-side union
// support lands.) Returns 0 on success, -1 if no entry exists.
__attribute__((always_inline))
static inline long t_unmount(unsigned long target_path_id) {
    register long x0 __asm__("x0") = (long)target_path_id;
    register long x8 __asm__("x8") = T_SYS_UNMOUNT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_pipe — create a connected Spoor pair, install both as KOBJ_SPOOR
// handles in the calling process's table. Returns 0 on success with
// *out_rd_fd / *out_wr_fd populated; -1 on table-full / OOM.
__attribute__((always_inline))
static inline long t_pipe(long *out_rd_fd, long *out_wr_fd) {
    register long x0 __asm__("x0");
    register long x1 __asm__("x1");
    register long x8 __asm__("x8") = T_SYS_PIPE;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0), "=r"(x1)
        : "r"(x8)
        : "memory", "cc"
    );
    if (x0 < 0) return -1;
    *out_rd_fd = x0;
    *out_wr_fd = x1;
    return 0;
}

// t_read — read up to `len` bytes from `fd` into `buf`. Returns bytes
// read (>0), 0 on EOF, -1 on error. Per-call cap is 4096 bytes
// (kernel-side SYS_RW_MAX); userspace loops for larger transfers.
__attribute__((always_inline))
static inline long t_read(long fd, void *buf, size_t len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x8 __asm__("x8") = T_SYS_READ;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_write — write up to `len` bytes from `buf` to `fd`. Returns bytes
// written (>=0), -1 on error.
__attribute__((always_inline))
static inline long t_write(long fd, const void *buf, size_t len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x8 __asm__("x8") = T_SYS_WRITE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_close — release the handle at `fd`. For KOBJ_SPOOR handles the
// kernel's release path routes through spoor_clunk (sets pipe EOF +
// wakes the other side per P5-pipe-blocking). Returns 0 on success,
// -1 on invalid fd.
__attribute__((always_inline))
static inline long t_close(long fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = T_SYS_CLOSE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_dup — duplicate `oldfd` with possibly-reduced rights into a new
// handle slot. `new_rights` MUST be a subset of oldfd's rights —
// elevation is rejected by the kernel's RightsCeiling enforcement.
// Returns the new fd (>=0) on success, -1 on invalid oldfd / rights
// elevation / table-full.
__attribute__((always_inline))
static inline long t_dup(long oldfd, unsigned long new_rights) {
    register long x0 __asm__("x0") = oldfd;
    register long x1 __asm__("x1") = (long)new_rights;
    register long x8 __asm__("x8") = T_SYS_DUP;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// =============================================================================
// P5-corvus-syscalls: v1.0 hardening syscalls (CORVUS-DESIGN.md §4.1.1).
// =============================================================================

// t_mlockall — pin all currently-mapped + future-mapped pages. Caller
// must hold T_RIGHT-equivalent CAP_LOCK_PAGES. Returns 0 on success,
// -1 on missing cap. Sets PROC_FLAG_MLOCKED on the calling Proc.
__attribute__((always_inline))
static inline long t_mlockall(unsigned long flags) {
    register long x0 __asm__("x0") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_MLOCKALL;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_set_dumpable — control core-dump permission. One-way to 0:
// t_set_dumpable(0) sets PROC_FLAG_NODUMP; t_set_dumpable(1) on a
// Proc that has the flag set is REFUSED. Returns 0 / -1.
__attribute__((always_inline))
static inline long t_set_dumpable(unsigned long dumpable) {
    register long x0 __asm__("x0") = (long)dumpable;
    register long x8 __asm__("x8") = T_SYS_SET_DUMPABLE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_set_traceable — control debug-Spoor attach permission. One-way to 0.
__attribute__((always_inline))
static inline long t_set_traceable(unsigned long traceable) {
    register long x0 __asm__("x0") = (long)traceable;
    register long x8 __asm__("x8") = T_SYS_SET_TRACEABLE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_explicit_bzero — compiler-barrier'd memset to zero of `len` bytes
// starting at `buf`. Per-call cap is SYS_RW_MAX (4096); loop for
// larger buffers. Returns 0 on success, -1 on user-VA validation
// failure. The kernel's per-byte uaccess_store_u8 path is the
// barrier — the compiler cannot elide the writes across the syscall
// boundary.
__attribute__((always_inline))
static inline long t_explicit_bzero(void *buf, size_t len) {
    register long x0 __asm__("x0") = (long)(unsigned long)buf;
    register long x1 __asm__("x1") = (long)len;
    register long x8 __asm__("x8") = T_SYS_EXPLICIT_BZERO;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_getrandom — read `len` random bytes into `buf` from the kernel
// CSPRNG. Caller must hold CAP_CSPRNG_READ. Per-call cap is SYS_RW_MAX
// (4096); loop for larger buffers. Returns bytes read (= len on
// success) or -1 on: missing cap / bad user-VA / CSPRNG unseeded /
// CSPRNG hardware fault.
__attribute__((always_inline))
static inline long t_getrandom(void *buf, size_t len, unsigned long flags) {
    register long x0 __asm__("x0") = (long)(unsigned long)buf;
    register long x1 __asm__("x1") = (long)len;
    register long x2 __asm__("x2") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_GETRANDOM;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn — load the named binary from the boot initrd (devramfs) and
// rfork(RFPROC) a child Proc that exec_setup's it. `name` need not be
// NUL-terminated; `name_len` is authoritative and must be in
// 1..T_SPAWN_NAME_MAX. The child inherits no capabilities at v1.0.
// Returns the child PID (>0) on success, -1 on: bad name buffer /
// missing binary / blob too large / OOM. Parent uses t_wait_pid to
// reap when the child exits.
__attribute__((always_inline))
static inline long t_spawn(const char *name, size_t name_len) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x8 __asm__("x8") = T_SYS_SPAWN;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_wait_pid — block until one of the caller's children enters ZOMBIE
// and reap it. If `status_out` is non-NULL, the child's exit_status is
// written there. Returns the reaped PID on success, -1 immediately if
// the caller has no children at all. Plan 9 wait(2) shape (no PID
// selector at v1.0).
__attribute__((always_inline))
static inline long t_wait_pid(int *status_out) {
    register long x0 __asm__("x0") = (long)(unsigned long)status_out;
    register long x8 __asm__("x8") = T_SYS_WAIT_PID;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_with_fds — like t_spawn, but pre-install the listed fds in
// the child's handle table at slots 0..fd_count-1. Each fd must be an
// open KOBJ_SPOOR handle in the caller; fd_count ≤ T_SPAWN_MAX_FDS.
// The parent retains its own holds on each fd; the child gets its own
// independent ref (caller-side refcount unchanged).
//
// Returns the child PID (>0) on success, -1 on: bad name buffer /
// missing binary / blob too large / fd_count > T_SPAWN_MAX_FDS /
// fd_list bound violation / any listed fd is not KOBJ_SPOOR / OOM.
__attribute__((always_inline))
static inline long t_spawn_with_fds(const char *name, size_t name_len,
                                    const unsigned int *fds, size_t fd_count) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)(unsigned long)fds;
    register long x3 __asm__("x3") = (long)fd_count;
    register long x8 __asm__("x8") = T_SYS_SPAWN_WITH_FDS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_with_caps — like t_spawn, but the child's caps are
// `caller's caps & cap_mask`. ARCH I-2 / I-6 monotonic-reduction is
// enforced structurally (the kernel-internal rfork_with_caps's AND
// can only reduce, never elevate). Setting bits in cap_mask that the
// caller doesn't hold has no effect — those bits are silently
// dropped at the AND.
//
// v1.0 cap bits (mirror kernel/include/thylacine/caps.h):
//   CAP_HW_CREATE   (1<<0) — hardware resource creation
//   CAP_LOCK_PAGES  (1<<1) — SYS_MLOCKALL
//   CAP_CSPRNG_READ (1<<2) — SYS_GETRANDOM
//
// Returns the child PID (>0) on success, -1 on the same conditions
// as t_spawn (bad name / missing binary / blob too large / OOM).
__attribute__((always_inline))
static inline long t_spawn_with_caps(const char *name, size_t name_len,
                                     unsigned long cap_mask) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)cap_mask;
    register long x8 __asm__("x8") = T_SYS_SPAWN_WITH_CAPS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_full — combination of t_spawn_with_fds + t_spawn_with_caps:
// child inherits the listed Spoor fds at slots 0..fd_count-1 AND gets
// cap-subset (caller's caps & cap_mask). Both inheritance modes apply
// independently with the same constraints and refcount discipline as
// the single-purpose variants. Used at P5-corvus-bringup where joey
// spawns /sbin/corvus with a pipe pair + CAP_LOCK_PAGES + CAP_CSPRNG_READ.
//
// Returns the child PID (>0) on success, -1 on the union of conditions
// from t_spawn_with_fds + t_spawn_with_caps.
__attribute__((always_inline))
static inline long t_spawn_full(const char *name, size_t name_len,
                                const unsigned int *fds, size_t fd_count,
                                unsigned long cap_mask) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)(unsigned long)fds;
    register long x3 __asm__("x3") = (long)fd_count;
    register long x4 __asm__("x4") = (long)cap_mask;
    register long x8 __asm__("x8") = T_SYS_SPAWN_FULL;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_with_perms — extends t_spawn_full with a `perm_flags` bitmask
// that the kernel stamps on the child Proc atomically inside the spawn
// thunk (BEFORE the child's exec_setup, so the child never observes the
// un-stamped intermediate state). `perm_flags` is the bitwise-OR of
// T_SPAWN_PERM_* bits.
//
// Any nonzero perm_flags bit requires the calling Proc to be console-
// attached (the v1.0 local-console trust anchor). joey IS console-
// attached; an ordinary Proc that wandered into this call is rejected.
//
// At v1.0 the only defined bit is T_SPAWN_PERM_MAY_POST_SERVICE, used
// by joey to grant /sbin/corvus the right to call SYS_POST_SERVICE
// ("corvus"). The bit is NOT a cap (rfork would propagate caps); kernel-
// stamped at spawn so it cannot cross an rfork boundary.
//
// Returns the child PID (>0) on success, -1 on:
//   - perm_flags has unknown bits
//   - any perm_flags bit set AND caller is not console-attached
//   - any condition that t_spawn_full returns -1 on
__attribute__((always_inline))
static inline long t_spawn_with_perms(const char *name, size_t name_len,
                                      const unsigned int *fds, size_t fd_count,
                                      unsigned long cap_mask,
                                      unsigned long perm_flags) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)(unsigned long)fds;
    register long x3 __asm__("x3") = (long)fd_count;
    register long x4 __asm__("x4") = (long)cap_mask;
    register long x5 __asm__("x5") = (long)perm_flags;
    register long x8 __asm__("x8") = T_SYS_SPAWN_WITH_PERMS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_dma_map — install a user-VA mapping for a KObj_DMA handle and return
// the underlying PA. The PTEs are installed lazily on first access
// (demand-paging). Returns the buffer's PA (always non-negative since
// PA fits in 40 bits at v1.0) on success, -1 on validation failure
// (cap-missing, bad handle, prot validation, vaddr alignment, OOM).
//
// The driver embeds the returned PA into device-visible descriptors
// (e.g., VirtIO virtqueue desc.addr fields). Pinned by
// HwHandleImpliesCap + burrow.tla::NoUseAfterFree.
__attribute__((always_inline))
static inline long t_dma_map(long h, unsigned long vaddr, unsigned long prot) {
    register long x0 __asm__("x0") = h;
    register long x1 __asm__("x1") = (long)vaddr;
    register long x2 __asm__("x2") = (long)prot;
    register long x8 __asm__("x8") = T_SYS_DMA_MAP;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_poll — block until one of `fds` (a slice of `struct pollfd`)
// becomes ready, or `timeout_ms` elapses. `timeout_ms`:
//   < 0 → block indefinitely;
//   = 0 → return immediately after the first scan;
//   > 0 → block for at most `timeout_ms` milliseconds.
//
// Returns the number of pollfds with `revents != 0` (≥ 0), 0 on
// timeout, or -1 on validation failure.
//
// libt callers consume `struct pollfd` + the T_POLL* event bits from
// <thylacine/poll.h>. The numeric constants match Linux for the future
// musl shim.
__attribute__((always_inline))
static inline long t_poll(void *fds, unsigned long nfds, long timeout_ms) {
    register long x0 __asm__("x0") = (long)(unsigned long)fds;
    register long x1 __asm__("x1") = (long)nfds;
    register long x2 __asm__("x2") = timeout_ms;
    register long x8 __asm__("x8") = 29; // T_SYS_POLL
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_srv_connect — open a client-side /srv connection to `name`, walking
// one path component `path` (typically `"ctl"`). The kernel mints a
// SrvConn, drives the full handshake (Tversion + Tattach + Twalk(path)
// + Tlopen) on the caller's behalf, and returns a KObj_Srv client
// handle. Subsequent t_read / t_write on the returned handle translate
// to Tread / Twrite at the open fid.
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
__attribute__((always_inline))
static inline long t_srv_connect(const char *name, size_t name_len,
                                 const char *path, size_t path_len) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)(unsigned long)path;
    register long x3 __asm__("x3") = (long)path_len;
    register long x8 __asm__("x8") = T_SYS_SRV_CONNECT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_cap_grant — register a pending CAP_HOSTOWNER grant for `target_stripes`
// with the kernel `cap` device (CORVUS-DESIGN.md §5.5.1; P5-hostowner-b-b).
// Caller must hold CAP_GRANT_HOSTOWNER. Returns 0 on success, -1 on gate
// fail / bad args / table full. At v1.0 only CAP_HOSTOWNER is grantable.
__attribute__((always_inline))
static inline long t_cap_grant(unsigned long cap_mask,
                               unsigned long target_stripes) {
    register long x0 __asm__("x0") = (long)cap_mask;
    register long x1 __asm__("x1") = (long)target_stripes;
    register long x8 __asm__("x8") = T_SYS_CAP_GRANT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_walk_open — walk a single path component from `spoor_fd` and open
// the result, returning a new opened KOBJ_SPOOR fd. The minimum walk-
// through-mount primitive at v1.0; composes with t_attach_9p to read a
// file served by a 9P server (P5-stratumd-stub-bringup-e1).
//
// Single component only — `name` must not contain '/' or '\0', and must
// not be "." or "..". Multi-component path resolution lands with the
// production open() syscall. Pass omode = T_OREAD / T_OWRITE / T_ORDWR
// (optionally OR'd with T_OTRUNC); other bits are rejected.
//
// The caller must hold RIGHT_READ on spoor_fd. The returned fd has
// R | W | TRANSFER — the server enforces the actual omode envelope.
//
// Returns the new fd (>=0) on success, -1 on:
//   - spoor_fd not KOBJ_SPOOR / missing RIGHT_READ
//   - the backing Dev has no walk or no open vtable op
//   - name_len == 0 or > 64
//   - name contains '/' / '\0' / equals "." / equals ".."
//   - omode has unrecognized bits
//   - server-side Rlerror on Twalk or Tlopen
//   - kernel-side OOM / handle table full
__attribute__((always_inline))
static inline long t_walk_open(long spoor_fd, const char *name,
                                size_t name_len, unsigned long omode) {
    register long x0 __asm__("x0") = spoor_fd;
    register long x1 __asm__("x1") = (long)(unsigned long)name;
    register long x2 __asm__("x2") = (long)name_len;
    register long x3 __asm__("x3") = (long)omode;
    register long x8 __asm__("x8") = T_SYS_WALK_OPEN;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_chroot — stamp the calling Proc's Territory root_spoor to the Spoor
// named by `spoor_fd`. After this call, t_walk_open(T_WALK_OPEN_FROM_ROOT,
// name, ...) walks from this Spoor. The caller may close `spoor_fd`
// afterward; the Territory keeps its own ref on the underlying Spoor
// until a subsequent t_chroot replaces it (releasing the old) or until
// the calling Proc exits (Territory destruction releases it).
// P5-stratumd-stub-bringup-e2.
//
// Idempotent — t_chroot to the same Spoor a second time is a 0 no-op.
//
// Caller must hold T_RIGHT_READ on spoor_fd (the same gate SYS_MOUNT
// applies — a chroot target's only purpose is to serve as a walk
// source).
//
// Returns 0 on success, -1 on:
//   - spoor_fd not KOBJ_SPOOR / out-of-range / missing T_RIGHT_READ
//   - caller has no Territory (kernel invariant; defense-in-depth)
__attribute__((always_inline))
static inline long t_chroot(long spoor_fd) {
    register long x0 __asm__("x0") = spoor_fd;
    register long x8 __asm__("x8") = T_SYS_CHROOT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_cap_use — redeem a pending cap grant for the caller's own stripes.
// Caller must hold PROC_FLAG_CONSOLE_ATTACHED and have a non-expired
// pending grant for its stripes with a matching cap_mask. On success the
// caller's caps gains `cap_mask`; the grant is consumed (one-shot).
// Returns 0 on success, -1 on gate fail / no pending grant / mismatch.
__attribute__((always_inline))
static inline long t_cap_use(unsigned long cap_mask) {
    register long x0 __asm__("x0") = (long)cap_mask;
    register long x8 __asm__("x8") = T_SYS_CAP_USE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_set_tid_address — register the calling thread's tid address and
// return its tid. A C runtime (pouch) calls this at thread startup; a
// Thylacine-native program rarely needs it directly. `tidptr` is now
// STORED on the Thread (P6-pouch-threads sub-chunk 9): on thread exit
// the kernel atomically zeroes *tidptr + torpor-wakes on it so a
// joiner observes the death. `tidptr == 0` is the "unset" sentinel.
//
// Returns the calling thread's tid (positive int — a per-Thread monotonic
// allocation; for the main Thread of a Proc this is NOT the pid in
// general). Returns -1 on user-VA validation failure (non-zero tidptr
// that is not 4-byte-aligned or outside the user-VA bound).
__attribute__((always_inline))
static inline long t_set_tid_address(void *tidptr) {
    register long x0 __asm__("x0") = (long)tidptr;
    register long x8 __asm__("x8") = T_SYS_SET_TID_ADDRESS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// P6-pouch-threads (sub-chunk 9): spawn an EL0 Thread in the caller's Proc.
//
// `entry`: function pointer for the new Thread's EL0 entry — invoked with
//          x0 = arg per AAPCS64. The function must NOT return; it should
//          call t_thread_exit (or pouch's pthread_exit) to terminate
//          cleanly.
// `sp_top`: top of the new Thread's user stack. Must be 16-byte aligned
//           (AAPCS64 ABI). The Thread runs on this stack until exit.
// `arg`:    opaque value passed as x0 to `entry`.
// `tls`:    initial TPIDR_EL0 value (TLS base). 0 = no TLS (entry must
//           install one before any TLS deref).
//
// Returns the new Thread's tid (positive int) on success, or one of the
// Linux/musl-numeric -errnos:
//   -EINVAL (-22)   bad alignment / out-of-bound entry / sp / tls
//   -ENOMEM (-12)   Thread / kstack alloc fail
__attribute__((always_inline))
static inline long t_thread_spawn(void (*entry)(void *), void *sp_top,
                                  void *arg, void *tls) {
    register long x0 __asm__("x0") = (long)(unsigned long)entry;
    register long x1 __asm__("x1") = (long)(unsigned long)sp_top;
    register long x2 __asm__("x2") = (long)(unsigned long)arg;
    register long x3 __asm__("x3") = (long)(unsigned long)tls;
    register long x8 __asm__("x8") = T_SYS_THREAD_SPAWN;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// P6-pouch-threads (sub-chunk 9): exit the calling Thread. NEVER returns.
//
// The kernel:
//   1. If t_set_tid_address registered a non-zero tidptr on this Thread,
//      atomically zeroes the tidptr word + torpor-wakes UINT32_MAX
//      threads on it (pthread_join handoff).
//   2. Marks self EXITING.
//   3. If this is the LAST live Thread in the Proc, transitions the
//      Proc to ZOMBIE with exit_status = 0 (mirrors t_exits(0)).
//   4. sched(); never returns.
//
// pouch's pthread_exit wraps this; native callers (rare) can use it
// directly.
__attribute__((always_inline, noreturn))
static inline void t_thread_exit(void) {
    register long x8 __asm__("x8") = T_SYS_THREAD_EXIT;
    __asm__ volatile (
        "svc #0"
        :
        : "r"(x8)
        : "memory", "cc"
    );
    __builtin_unreachable();
}

#endif // THYLA_SYSCALL_H
