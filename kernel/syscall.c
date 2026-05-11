// Userspace syscall dispatch implementation (P3-Ec).
//
// At v1.0 P3-Ec the syscall surface is intentionally tiny — exits +
// puts. Just enough for an EL0 thread to signal "I ran, and here's the
// result" to the kernel test harness. Phase 5+ adds the full syscall
// surface; each syscall lands in its own TU.

#include <thylacine/syscall.h>
#include <thylacine/caps.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/proc.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

#include "../arch/arm64/exception.h"
#include "../arch/arm64/uart.h"

// =============================================================================
// SYS_EXITS — terminate calling process.
// =============================================================================
//
// AArch64 ABI: x0 = exit status (0 → "ok"; non-zero → "fail").
//
// At v1.0 P3-Ec we map the integer status to the existing kernel
// exits() string-based convention:
//
//   x0 == 0  → exits("ok")    → p->exit_status = 0
//   x0 != 0  → exits("fail")  → p->exit_status = 1
//
// Phase 5+ extends to a richer per-Proc exit_status u64 carrying the
// full integer payload.
//
// exits() is __attribute__((noreturn)); this helper inherits the
// no-return semantics. The user thread context is abandoned (its
// kernel stack with the exception_context on it stays around until
// wait_pid reaps via thread_free).
__attribute__((noreturn))
static void sys_exits_handler(u64 status) {
    if (status == 0) {
        exits("ok");
    } else {
        exits("fail");
    }
    // Unreachable — exits() is noreturn.
    extinction("sys_exits returned");
}

// =============================================================================
// SYS_PUTS — write `len` bytes to UART.
// =============================================================================
//
// AArch64 ABI: x0 = pointer to bytes; x1 = length.
//
// v1.0 sanity bounds:
//   - len <= 4096 (one page; reject larger as obvious garbage / reserved
//     for Phase 5+ where userspace uses iovec for larger writes).
//   - buf NULL rejected.
//   - buf + len must lie entirely within the user-VA half (TTBR0 range,
//     low VAs) — see SYS_PUTS_USER_VA_TOP. Closes R7 F127: without this,
//     EL0 can pass a kernel-half VA (TTBR1 range) and the kernel's
//     dereference walks via TTBR1 → reads kernel memory → leaks bytes
//     out the UART. PAN/SPAN are not configured at v1.0; the bound
//     check is the privilege boundary on this surface.
//
// Bytes are written one at a time via uart_putc. v1.0 doesn't validate
// the buffer's VMA presence within user-VA — if buf points outside any
// VMA, the read faults in the demand-paging path; if no VMA covers it,
// userland_demand_page returns FAULT_UNHANDLED_USER and the kernel
// extincts at exception_sync_lower_el. Phase 5+ adds copy_from_user-
// style validators that translate the fault into a -EFAULT return.

// User-VA top bound for syscall pointer validation. ARM64 4-KiB granule
// at v1.0 uses 48-bit VAs; user half is TTBR0-anchored at low VA. The
// EXEC_USER_STACK_TOP (0x8000_0000) sits well below this bound. Any VA
// >= USER_VA_TOP is in the kernel-VA-only region (TTBR1 range or
// reserved hole) and MUST NOT be dereferenced from a syscall handler.
#define SYS_PUTS_USER_VA_TOP    0x0001000000000000ull

static s64 sys_puts_handler(u64 buf_va, u64 len) {
    if (len == 0)            return 0;
    if (len > 4096)          return -1;
    if (buf_va == 0)         return -1;

    // R7 F127 close: reject kernel-half VA arguments. Overflow-safe:
    // if buf_va + len wraps past UINT64_MAX, that's also a reject.
    if (buf_va >= SYS_PUTS_USER_VA_TOP)              return -1;
    if (buf_va + len < buf_va)                        return -1;
    if (buf_va + len > SYS_PUTS_USER_VA_TOP)          return -1;

    const char *buf = (const char *)(uintptr_t)buf_va;
    for (u64 i = 0; i < len; i++) {
        uart_putc(buf[i]);
    }
    return (s64)len;
}

// =============================================================================
// SYS_MMIO_CREATE — allocate a KObj_MMIO handle for a PA range (P4-Ib).
// =============================================================================
//
// AArch64 ABI: x0 = pa, x1 = size, x2 = rights.
//
// Capability-gated per specs/handles.tla::HwHandleImpliesCap:
//   `caller->caps & CAP_HW_CREATE` must be non-zero. v1.0 only kproc
//   has this cap; rfork'd children inherit CAP_NONE.
//
// PA-range exclusivity per specs/handles.tla::HwResourceExclusive:
//   kobj_mmio_create rejects overlap with an existing claim.
//
// Returns: hidx_t (>=0) on success, -1 on EPERM / EINVAL / EBUSY /
// table-full / OOM.
static s64 sys_mmio_create_handler(u64 pa, u64 size, u64 rights) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // P4-Ib HwHandleImpliesCap: require CAP_HW_CREATE. The bug class
    // BuggyHwCreateNoCap is rejected here.
    // R9 F146 (P2) close: atomic load of p->caps. At v1.0 there's no
    // cross-thread writer of p->caps (no ReduceCaps syscall yet); the
    // plain load is currently safe. Using __atomic_load_n + ACQUIRE
    // future-proofs against Phase 5+ paths where another thread may
    // mutate caps. Negligible cost on aarch64 (ldar single instruction).
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;

    // Validate rights early so a buggy caller doesn't allocate a KObj
    // we'll have to immediately free.
    if (rights == 0 || (rights & ~(u64)RIGHT_ALL))   return -1;

    // P4-Ib HwResourceExclusive enforced by kobj_mmio_create: returns
    // NULL on overlap, bad alignment, size 0, OOM, or table-full.
    struct KObj_MMIO *k = kobj_mmio_create(pa, (size_t)size);
    if (!k)                                          return -1;

    hidx_t h = handle_alloc(p, KOBJ_MMIO, (rights_t)rights, k);
    if (h < 0) {
        // Rollback the kobj_mmio_create. The PA-range claim is held
        // until kobj_mmio_unref drops the refcount; we MUST release
        // it so the caller's retry (or another driver's create) can
        // succeed.
        kobj_mmio_unref(k);
        return -1;
    }
    return (s64)h;
}

// =============================================================================
// SYS_IRQ_CREATE — allocate a KObj_IRQ handle for an INTID (P4-Ib).
// =============================================================================
//
// AArch64 ABI: x0 = intid, x1 = rights.
//
// Same cap-gating + exclusivity semantics as SYS_MMIO_CREATE.
static s64 sys_irq_create_handler(u64 intid, u64 rights) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // R9 F146 (P2) close: atomic load of p->caps. At v1.0 there's no
    // cross-thread writer of p->caps (no ReduceCaps syscall yet); the
    // plain load is currently safe. Using __atomic_load_n + ACQUIRE
    // future-proofs against Phase 5+ paths where another thread may
    // mutate caps. Negligible cost on aarch64 (ldar single instruction).
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;
    if (rights == 0 || (rights & ~(u64)RIGHT_ALL))   return -1;
    if (intid > (u64)0xFFFFFFFFu)                    return -1;

    // R9 F145 (P1) close: SGI/PPI (intid < 32) are kernel-only at
    // v1.0. SGI 0..15 carry IPIs (resched, future shootdown). PPI
    // 16..31 host per-CPU peripherals (timer at 30, virt timer).
    // SGI disable is per-CPU at the redistributor; the global claim
    // table can't represent the per-CPU semantics correctly, and the
    // kernel needs exclusive control over these for scheduler /
    // timer correctness. Drivers register for SPIs (intid >= 32) only.
    // R9 F142 (P0) reinforcement: even with the irqfwd_init kernel
    // reservation (above), this syscall-layer check makes the
    // restriction explicit at the API boundary.
    if (intid < 32)                                  return -1;

    // INTID exclusivity enforced by kobj_irq_create's intid_try_claim
    // (P4-Ib addition); returns NULL on already-claimed / OOM /
    // gic_attach failure.
    struct KObj_IRQ *k = kobj_irq_create((u32)intid);
    if (!k)                                          return -1;

    hidx_t h = handle_alloc(p, KOBJ_IRQ, (rights_t)rights, k);
    if (h < 0) {
        kobj_irq_unref(k);
        return -1;
    }
    return (s64)h;
}

// =============================================================================
// SYS_IRQ_WAIT — block until at least one IRQ has fired since last wait.
// =============================================================================
//
// AArch64 ABI: x0 = handle index.
//
// Returns: count of collapsed IRQs that fired (always >= 1), or
// (u64)-1 on bad handle / wrong kind / missing right.
static s64 sys_irq_wait_handler(u64 hraw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // R9 F144 (P1) close: removed the redundant `hraw > PROC_HANDLE_MAX`
    // pre-check (which was an off-by-one — should have been `>=`).
    // handle_get is the canonical bound-checker: `h < 0 ||
    // h >= PROC_HANDLE_MAX → NULL`. Casting u64 → hidx_t (int) safely
    // saturates large u64 values to negative ints, which handle_get
    // rejects via `h < 0`.
    struct Handle *slot = handle_get(p, (hidx_t)hraw);
    if (!slot)                                       return -1;
    if (slot->kind != KOBJ_IRQ)                      return -1;

    // RIGHT_SIGNAL gates waits on KObj_IRQ — a holder without SIGNAL
    // can pass the handle around (in a future Phase 5+ transfer surface
    // — currently forbidden) but not consume IRQs.
    if ((slot->rights & RIGHT_SIGNAL) == 0)          return -1;

    struct KObj_IRQ *k = (struct KObj_IRQ *)slot->obj;
    if (!k)                                          return -1;

    // R9 F143 (P1) close: bump the ref BEFORE releasing the handle-table
    // lookup (effectively immediately — there's no lock to release at
    // v1.0, but the principle holds) and BEFORE entering the blocking
    // sleep inside kobj_irq_wait. Without this, a concurrent
    // handle_close on this slot would call kobj_irq_unref(k), drop
    // the ref to 0, and free k while we're still sleeping on
    // &k->rendez — UAF on wake.
    //
    // Pairs with the kobj_irq_unref below after wait returns. The
    // handle_close path's unref balances with the original ref=1 from
    // kobj_irq_create; THIS ref is purely the syscall's borrow.
    kobj_irq_ref(k);
    u32 count = kobj_irq_wait(k);
    kobj_irq_unref(k);
    return (s64)count;
}

// =============================================================================
// Dispatch entry.
// =============================================================================

void syscall_dispatch(struct exception_context *ctx) {
    u64 nr = ctx->regs[8];

    switch (nr) {
    case SYS_EXITS:
        // Never returns. Kernel exits() → sched() picks another thread.
        // The exception_context stays on the EXITING thread's kstack
        // until wait_pid → thread_free.
        sys_exits_handler(ctx->regs[0]);

    case SYS_PUTS:
        ctx->regs[0] = (u64)sys_puts_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_MMIO_CREATE:
        ctx->regs[0] = (u64)sys_mmio_create_handler(ctx->regs[0],
                                                    ctx->regs[1],
                                                    ctx->regs[2]);
        return;

    case SYS_IRQ_CREATE:
        ctx->regs[0] = (u64)sys_irq_create_handler(ctx->regs[0],
                                                   ctx->regs[1]);
        return;

    case SYS_IRQ_WAIT:
        ctx->regs[0] = (u64)sys_irq_wait_handler(ctx->regs[0]);
        return;

    default:
        // Unknown syscall. Phase 5+ delivers SIGSYS-equivalent note;
        // v1.0 returns -1 (ENOSYS) and lets userspace decide.
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
}
