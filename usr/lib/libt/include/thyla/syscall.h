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
};

// SYS_GETRANDOM flags (mirror kernel/include/thylacine/syscall.h).
#define T_GRND_NONBLOCK   1u

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

#endif // THYLA_SYSCALL_H
