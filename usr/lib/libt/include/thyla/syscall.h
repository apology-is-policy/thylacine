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
};

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
