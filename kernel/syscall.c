// Userspace syscall dispatch implementation (P3-Ec).
//
// At v1.0 P3-Ec the syscall surface is intentionally tiny — exits +
// puts. Just enough for an EL0 thread to signal "I ran, and here's the
// result" to the kernel test harness. Phase 5+ adds the full syscall
// surface; each syscall lands in its own TU.

#include <thylacine/syscall.h>
#include <thylacine/9p_attach.h>
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/burrow.h>
#include <thylacine/caps.h>
#include <thylacine/dev.h>
#include <thylacine/dev9p.h>
#include <thylacine/devramfs.h>
#include <thylacine/devcap.h>
#include <thylacine/devsrv.h>
#include <thylacine/dma_handle.h>
#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/notes.h>
#include <thylacine/page.h>
#include <thylacine/pipe.h>
#include <thylacine/poll.h>
#include <thylacine/perm.h>
#include <thylacine/proc.h>
#include <thylacine/random.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/torpor.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../arch/arm64/exception.h"
#include "../arch/arm64/timer.h"
#include "../arch/arm64/uaccess.h"
#include "../arch/arm64/uart.h"
#include "../mm/slub.h"

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

// SYS_EXIT_GROUP / POSIX exit_group(2) (ARCH §7.9.1, invariant I-24).
// Terminate the WHOLE Proc -- cascade peer-Thread termination -- not just the
// calling Thread. proc_group_terminate flags the Proc + wakes torpor sleepers
// + IPI-kicks running peers so each self-exits at its EL0-return die-check;
// then this Thread exits via thread_exit_self, which honors the recorded
// group_exit_msg for the last-Thread-out ZOMBIE status. A single-thread Proc
// is equivalent to exits(status). Replaces the v1.0 path where _Exit /
// exit_group routed to SYS_EXITS and extincted the kernel on live peers.
__attribute__((noreturn))
static void sys_exit_group_handler(u64 status) {
    struct Thread *t = current_thread();
    struct Proc   *p = (t && t->magic == THREAD_MAGIC) ? t->proc : NULL;
    const char *msg = (status == 0) ? "ok" : "fail";
    if (p && p->magic == PROC_MAGIC) {
        // proc_group_terminate's universal death-wake walks p->threads, which
        // requires g_proc_table_lock (#811, ARCH §8.8.1). Acquire it around the
        // call and RELEASE before thread_exit_self -- thread_exit_self
        // re-acquires it for the last-out ZOMBIE transition (spinlocks are not
        // recursive).
        irq_state_t s = proc_table_lock_acquire();
        proc_group_terminate(p, msg);
        proc_table_lock_release(s);
    }
    // Exit the caller (a Thread of p). thread_exit_self validates current /
    // proc state + extincts on kproc / corruption, mirroring sys_exits_handler.
    thread_exit_self();
    // Unreachable -- thread_exit_self is noreturn.
    extinction("sys_exit_group returned");
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
// R12-uaccess: bytes are read one at a time via uaccess_load_u8, the
// kernel-side fault-recoverable primitive (arch/arm64/uaccess.S). The
// asm primitive's ldrb fires a translation fault if the user page is
// in a VMA but not yet PTE-installed; exception_sync_curr_el catches
// the fault, calls userland_demand_page to install the PTE, and
// resumes the load. If no VMA covers the page (or any other
// unrecoverable condition), the fault dispatcher transfers control
// to the primitive's fixup label which returns -1, and SYS_PUTS
// propagates that as its overall -1 return. Pre-R12, userspace
// crates carried pretouch_rodata_pages() to read each .rodata page
// from EL0 before calling SYS_PUTS — that workaround is now retired.

// R12-uaccess F210 close: SYS_PUTS now uses the canonical
// UACCESS_USER_VA_TOP (= USER_VA_TOP = 1ull << 47) as the syscall-layer
// bound, matching the dispatcher's user-VA recovery range. Pre-fix,
// SYS_PUTS_USER_VA_TOP was 0x0001000000000000ull (= 1ull << 48): EL0
// could pass buf_va in [2^47, 2^48), pass the syscall-level check,
// fault inside uaccess_load_u8's ldrb, and the dispatcher's
// `fi.vaddr < UACCESS_USER_VA_TOP` check would FAIL (since the FAR
// was ≥ 2^47), routing to arch_fault_handle's "unhandled kernel
// translation fault" extinction. EL0 thus extincted the kernel
// without any capability. Closes also R7 F127 with the tighter
// bound. ARM IHI 0487 D5.2.4: with 48-bit VAs and no TBI, valid
// TTBR0 addresses occupy bits [46:0]; bit 47 is the TTBR selector.

static s64 sys_puts_handler(u64 buf_va, u64 len) {
    if (len == 0)            return 0;
    if (len > 4096)          return -1;
    if (buf_va == 0)         return -1;

    // R7 F127 close + R12-uaccess F210 close: reject any VA outside
    // the user half. Overflow-safe: if buf_va + len wraps past
    // UINT64_MAX, that's also a reject. The bound is identical to
    // burrow_map's USER_VA_TOP and the uaccess dispatcher's
    // UACCESS_USER_VA_TOP; if any of the three drift, the
    // _Static_assert in arch/arm64/uaccess.c trips at build time.
    if (buf_va >= UACCESS_USER_VA_TOP)               return -1;
    if (buf_va + len < buf_va)                        return -1;
    if (buf_va + len > UACCESS_USER_VA_TOP)           return -1;

    // R12-uaccess: read one byte at a time via uaccess_load_u8. The
    // asm primitive returns 0 on success (byte in `c`) or -1 if the
    // fault dispatcher couldn't demand-page the user VA (no VMA /
    // perm denied / OOM during sub-table alloc). On -1 we propagate
    // the failure to the SYS_PUTS caller and report how many bytes
    // were successfully written via the return path's negative
    // convention. v1.0 reports -1 (EFAULT-equivalent) regardless of
    // partial progress; Phase 5+ may refine to a partial-write byte
    // count once the syscall ABI gains a richer error/return surface.
    for (u64 i = 0; i < len; i++) {
        u8 c;
        if (uaccess_load_u8(buf_va + i, &c) != 0) return -1;
        uart_putc((char)c);
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
// SYS_MMIO_MAP — install a user-VA mapping for a KObj_MMIO handle (P4-Ic2).
// =============================================================================
//
// AArch64 ABI: x0 = handle index, x1 = vaddr, x2 = prot.
//
// Validates the handle (KOBJ_MMIO + RIGHT_MAP), bounds the requested
// prot by the handle's rights (a holder without RIGHT_WRITE can't map
// RW), creates a BURROW_TYPE_MMIO Burrow wrapping the KObj_MMIO,
// installs a VMA via burrow_map, and drops the construction reference
// (transferring ownership to the VMA's mapping ref). The actual PTE
// installation happens lazily via userland_demand_page on first access.
//
// Returns 0 on success, -1 on:
//   - NULL Proc / corrupted Proc (handler entry guard)
//   - cap-missing CAP_HW_CREATE (defense-in-depth — spec invariant
//     HwHandleImpliesCap already requires the cap to hold the handle)
//   - bad handle (out of range, wrong kind, missing RIGHT_MAP)
//   - prot exceeds handle rights (e.g., WRITE without RIGHT_WRITE)
//   - prot has EXEC set (MMIO is not executable; ARM ARM B2.7.2)
//   - prot == 0 (must have at least READ)
//   - burrow_create_mmio OOM
//   - burrow_map failure (overlap with existing VMA, vaddr misalign,
//     overflow, SLUB OOM for the Vma struct)
static s64 sys_mmio_map_handler(u64 hraw, u64 vaddr, u64 prot_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Defense-in-depth: hw-handle ownership implies CAP_HW_CREATE per
    // spec invariant HwHandleImpliesCap. If a future path violates the
    // invariant (handle held without cap), the syscall layer catches
    // it before the mapping is installed.
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;

    struct Handle *slot = handle_get(p, (hidx_t)hraw);
    if (!slot)                                       return -1;
    if (slot->kind != KOBJ_MMIO)                     return -1;
    if ((slot->rights & RIGHT_MAP) == 0)             return -1;

    // Bound requested prot by the handle's rights. R+W → handle must
    // have RIGHT_WRITE; R → handle must have RIGHT_READ. EXEC is
    // rejected entirely for MMIO mappings (device-memory PTEs aren't
    // architecturally executable in a useful way).
    u32 prot = (u32)prot_raw;
    if (prot == 0)                                   return -1;
    if (prot & ~(u32)(VMA_PROT_READ | VMA_PROT_WRITE)) return -1;
    if ((prot & VMA_PROT_WRITE) && !(slot->rights & RIGHT_WRITE)) return -1;
    if ((prot & VMA_PROT_READ)  && !(slot->rights & RIGHT_READ))  return -1;

    // R10 F155 (P2) close: AArch64 has no write-only AP encoding
    // (AP[2:1] = {00=RW EL1, 01=RW any, 10=RO EL1, 11=RO any} — no
    // W-only state per ARM ARM D5.4.1). A `prot=VMA_PROT_WRITE` only
    // request would result in a fully-RW PTE, breaking the rights
    // claim ("caller can write but not read this device"). Reject
    // the construct so the rights model and the actual PTE always
    // agree. Drivers that want write access MUST request R+W.
    if ((prot & VMA_PROT_WRITE) && !(prot & VMA_PROT_READ)) return -1;

    struct KObj_MMIO *km = (struct KObj_MMIO *)slot->obj;
    if (!km)                                         return -1;
    if (km->magic != KOBJ_MMIO_MAGIC)                return -1;

    // Create the Burrow. handle_count=1 is the construction reference.
    struct Burrow *b = burrow_create_mmio(km);
    if (!b)                                          return -1;

    // Install the VMA via burrow_map. On success, mapping_count is
    // incremented (matches anon flow); we then drop the construction
    // reference, transferring ownership to the VMA. On failure, drop
    // the construction reference which (since mapping_count is still
    // 0) triggers burrow_free_internal and releases the kobj_mmio ref.
    //
    // P6 #713 vma_lock audit F1: burrow_map walks + splices p->vmas
    // (vma_insert), so it MUST hold p->vma_lock -- same discipline as
    // SYS_BURROW_ATTACH. Without it a sibling thread's fault-path
    // vma_lookup (or another mapper) races this vma_insert. Lock order
    // vma_lock -> buddy zone->lock holds (burrow_unref-on-failure ->
    // free_pages). burrow_create_mmio stays outside (no VMA touch).
    spin_lock(&p->vma_lock);
    int rc = burrow_map(p, b, vaddr, km->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        spin_unlock(&p->vma_lock);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->vma_lock);
    return 0;
}

// =============================================================================
// SYS_DMA_CREATE — allocate a KObj_DMA handle for a contiguous DMA buffer (P4-Ic5b1b).
// =============================================================================
//
// AArch64 ABI: x0 = size, x1 = rights.
//
// Capability-gated per specs/handles.tla::HwHandleImpliesCap:
//   `caller->caps & CAP_HW_CREATE` must be non-zero (mirrors MMIO/IRQ).
//
// Size constraints: > 0, page-aligned at create-time (kobj_dma_create
// page-aligns up), <= KOBJ_DMA_MAX_SIZE (1 MiB at v1.0). The kernel
// chooses the PA via alloc_pages — distinct from MMIO where the caller
// specifies the PA.
//
// Returns: hidx_t (>=0) on success, -1 on EPERM / EINVAL / EBUSY /
// table-full / OOM.
static s64 sys_dma_create_handler(u64 size, u64 rights) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // HwHandleImpliesCap: require CAP_HW_CREATE. Acquire-fence load
    // matches the R9 F146 discipline used in sys_mmio_create /
    // sys_irq_create — future-proofs against Phase 5+ paths where a
    // peer thread may mutate caps.
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;

    // Validate rights early. Reject empty + reject bits outside
    // RIGHT_ALL. RIGHT_DMA is the natural marker for DMA-capable
    // handles but isn't structurally required at create time — the
    // userspace driver decides what it needs at map time.
    if (rights == 0 || (rights & ~(u64)RIGHT_ALL))   return -1;

    // Bound size against u64 → size_t conversion. size_t is 64-bit on
    // aarch64; the comparison is safe.
    if (size == 0)                                   return -1;

    struct KObj_DMA *k = kobj_dma_create((size_t)size);
    if (!k)                                          return -1;

    hidx_t h = handle_alloc(p, KOBJ_DMA, (rights_t)rights, k);
    if (h < 0) {
        // Rollback: release the page chunk back to buddy. Mirrors the
        // sys_mmio_create rollback for the same reason — the proc never
        // received a handle, so the construction reference must be
        // dropped here.
        kobj_dma_unref(k);
        return -1;
    }
    return (s64)h;
}

// =============================================================================
// SYS_DMA_MAP — install a user-VA mapping for a KObj_DMA handle (P4-Ic5b1b).
// =============================================================================
//
// AArch64 ABI: x0 = handle index, x1 = vaddr, x2 = prot.
//
// Validates the handle (KOBJ_DMA + RIGHT_MAP), bounds the requested
// prot by the handle's rights, creates a BURROW_TYPE_DMA Burrow wrapping
// the KObj_DMA, installs a VMA via burrow_map, drops the construction
// reference (transferring ownership to the VMA's mapping ref), and
// returns the underlying PA so the driver can embed it in device-visible
// descriptors.
//
// Returns: non-negative PA on success, -1 on failure. PA fits in 40 bits
// (TCR.IPS bound at v1.0); the s64 cast is safe — no valid PA has the
// sign bit set.
//
// Failure cases:
//   - NULL Proc / corrupted Proc.
//   - cap-missing CAP_HW_CREATE (defense-in-depth — HwHandleImpliesCap
//     already requires the cap to hold the handle).
//   - bad handle (out of range, wrong kind, missing RIGHT_MAP).
//   - prot exceeds handle rights (e.g., WRITE without RIGHT_WRITE).
//   - prot has EXEC set (DMA buffers are not executable; W^X invariant
//     I-12 — device data lives in these pages, never code).
//   - prot == 0 (must have at least READ).
//   - prot has WRITE without READ (AArch64 has no W-only PTE encoding;
//     mirrors the SYS_MMIO_MAP R10 F155 close).
//   - burrow_create_dma OOM.
//   - burrow_map failure (overlap with existing VMA, vaddr misalign,
//     overflow, SLUB OOM for the Vma struct).
static s64 sys_dma_map_handler(u64 hraw, u64 vaddr, u64 prot_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Defense-in-depth — hw-handle ownership implies CAP_HW_CREATE per
    // HwHandleImpliesCap. Mirror of the SYS_MMIO_MAP guard.
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;

    struct Handle *slot = handle_get(p, (hidx_t)hraw);
    if (!slot)                                       return -1;
    if (slot->kind != KOBJ_DMA)                      return -1;
    if ((slot->rights & RIGHT_MAP) == 0)             return -1;

    // Bound requested prot by the handle's rights. EXEC is rejected
    // unconditionally — DMA buffers carry data, never code (W^X
    // invariant I-12 + structural defense against ELF-loaded code
    // accidentally executing from a DMA buffer).
    u32 prot = (u32)prot_raw;
    if (prot == 0)                                   return -1;
    if (prot & ~(u32)(VMA_PROT_READ | VMA_PROT_WRITE)) return -1;
    if ((prot & VMA_PROT_WRITE) && !(slot->rights & RIGHT_WRITE)) return -1;
    if ((prot & VMA_PROT_READ)  && !(slot->rights & RIGHT_READ))  return -1;

    // AArch64 has no write-only PTE encoding (mirrors SYS_MMIO_MAP R10
    // F155): a `prot = WRITE` only request would map fully-RW, breaking
    // the rights claim. Reject so rights model and PTE always agree.
    if ((prot & VMA_PROT_WRITE) && !(prot & VMA_PROT_READ)) return -1;

    struct KObj_DMA *kd = (struct KObj_DMA *)slot->obj;
    if (!kd)                                         return -1;
    if (kd->magic != KOBJ_DMA_MAGIC)                 return -1;

    // Create the Burrow. handle_count=1 is the construction reference.
    struct Burrow *b = burrow_create_dma(kd);
    if (!b)                                          return -1;

    // Install the VMA via burrow_map. On success, mapping_count++. We
    // then drop the construction reference, transferring ownership to
    // the VMA. On failure, dropping the construction reference (with
    // mapping_count still 0) triggers burrow_free_internal → releases
    // the held kobj_dma ref → if it was the last ref, free_pages.
    //
    // P6 #713 vma_lock audit F1: burrow_map mutates p->vmas (vma_insert),
    // so it MUST hold p->vma_lock -- same discipline as SYS_BURROW_ATTACH /
    // SYS_MMIO_MAP. stratumd (multi-thread, CAP_HW_CREATE) maps its
    // virtio-blk DMA buffer here concurrently with sibling-thread faults.
    spin_lock(&p->vma_lock);
    int rc = burrow_map(p, b, vaddr, kd->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        spin_unlock(&p->vma_lock);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->vma_lock);

    // PA fits in 40 bits at v1.0 (TCR.IPS bound; mmu.c:668). The s64
    // cast is safe — no valid PA has the sign bit set.
    return (s64)kd->pa;
}

// =============================================================================
// SYS_PIPE — create a connected Spoor pair, install both as KOBJ_SPOOR
// handles in the caller's HandleTable (P5-fd-pipe).
// =============================================================================
//
// No userspace arguments. Returns the read-end fd in x0 and the
// write-end fd in x1. On failure returns x0 = -1 (and x1 unmodified;
// callers check x0 only).
//
// Discipline:
//   1. pipe_create() allocates the ring + two Spoors with ref=1 each.
//   2. handle_alloc takes ownership of each Spoor's ref. On success
//      the handle holds the ref; on Proc-exit / handle_close, the
//      handle-release path runs spoor_clunk (P5-fd-pipe wired
//      KOBJ_SPOOR into handle_release_obj).
//   3. On partial failure (second handle_alloc fails), the first
//      handle is closed (release-path spoor_clunks the first Spoor)
//      and the second Spoor is spoor_clunk'd directly.
//
// Rights: RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER. v1.0 grants
// the union so the caller can read, write, and pass the fds across
// 9P sessions when the transfer-via-9P path lands. Future
// implementations may pre-narrow rights per end (read-only for the
// read end; write-only for the write end) — at v1.0 the dev9p_read /
// dev9p_write checks (which look at is_read_end) provide the actual
// gating; handle rights are an additional gate, not the primary one.
//
// Exposed (non-static) for kernel-internal tests in test_sys_pipe.c.
// Returns 0 on success with *out_rd / *out_wr populated; -1 on
// failure with both Spoors clunked.
int sys_pipe_for_proc(struct Proc *p, hidx_t *out_rd, hidx_t *out_wr) {
    if (!p || !out_rd || !out_wr)                    return -1;

    struct Spoor *rd = NULL;
    struct Spoor *wr = NULL;
    if (pipe_create(&rd, &wr) < 0)                   return -1;

    rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;

    hidx_t fd_rd = handle_alloc(p, KOBJ_SPOOR, r, rd);
    if (fd_rd < 0) {
        spoor_clunk(rd);
        spoor_clunk(wr);
        return -1;
    }
    hidx_t fd_wr = handle_alloc(p, KOBJ_SPOOR, r, wr);
    if (fd_wr < 0) {
        // The first handle owns `rd` via handle_release_obj's
        // spoor_clunk. Closing it returns the Spoor to ref=0.
        handle_close(p, fd_rd);
        // The second Spoor was never installed — clunk directly.
        spoor_clunk(wr);
        return -1;
    }

    *out_rd = fd_rd;
    *out_wr = fd_wr;
    return 0;
}

static s64 sys_pipe_handler(u64 *out_rd, u64 *out_wr) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    hidx_t rd, wr;
    if (sys_pipe_for_proc(p, &rd, &wr) < 0)          return -1;
    *out_rd = (u64)rd;
    *out_wr = (u64)wr;
    return 0;
}

// =============================================================================
// SYS_READ / SYS_WRITE — byte I/O through a KOBJ_SPOOR fd (P5-fd-rw).
// =============================================================================
//
// AArch64 ABI: x0 = fd (hidx_t), x1 = buf_va (user-VA pointer),
//              x2 = len (bytes).
//
// SYS_READ:  routes through dev->read; copies kernel-scratch bytes
//            back to user-VA via uaccess_store_u8. Returns bytes read
//            (>=0; 0 on EOF), -1 on error.
// SYS_WRITE: copies user-VA bytes into kernel scratch via uaccess_load_u8;
//            routes through dev->write. Returns bytes written (>=0),
//            -1 on error.
//
// Length is capped at SYS_RW_MAX per call (4096 bytes; matches
// PIPE_BUF_SIZE). Userspace loops for larger transfers.
//
// Rights gate: SYS_READ requires RIGHT_READ on the handle; SYS_WRITE
// requires RIGHT_WRITE.

// Helper: look up an open KOBJ_SPOOR handle, validate rights. Returns
// the Spoor pointer or NULL on any failure.
static struct Spoor *sys_lookup_spoor(struct Proc *p, hidx_t h, rights_t required) {
    struct Handle *slot = handle_get(p, h);
    if (!slot)                                       return NULL;
    if (slot->kind != KOBJ_SPOOR)                    return NULL;
    if ((slot->rights & required) != required)       return NULL;
    return (struct Spoor *)slot->obj;
}

// Helper: look up an open r/w-capable handle (KOBJ_SPOOR or KOBJ_SRV
// with SRV_CONN_MAGIC) + validate rights. Returns the slot pointer or
// NULL. P5-corvus-srv-impl-b2 — bridges SYS_READ / SYS_WRITE between
// Spoor-vtable I/O (KOBJ_SPOOR) and the SrvConn-mediated 9P Tread /
// Twrite (KOBJ_SRV connection handles); the SRV_SERVICE_MAGIC handles
// are not r/w-able (a service registry slot is not a transport).
static struct Handle *sys_lookup_rw_handle(struct Proc *p, hidx_t h,
                                             rights_t required) {
    struct Handle *slot = handle_get(p, h);
    if (!slot)                                       return NULL;
    if ((slot->rights & required) != required)       return NULL;
    switch (slot->kind) {
    case KOBJ_SPOOR:
        return slot;
    case KOBJ_SRV: {
        if (!slot->obj)                              return NULL;
        u64 m = *(const u64 *)slot->obj;
        if (m != SRV_CONN_MAGIC)                     return NULL;
        return slot;
    }
    default:
        return NULL;
    }
}

// Common user-VA range check (NULL / overflow / past UACCESS bound).
static bool sys_validate_user_buf(u64 buf_va, u64 len) {
    if (len == 0)                                    return true;
    if (buf_va == 0)                                 return false;
    if (buf_va >= UACCESS_USER_VA_TOP)                return false;
    if (buf_va + len < buf_va)                        return false;
    if (buf_va + len > UACCESS_USER_VA_TOP)           return false;
    return true;
}

// Inner — testable with kernel-side buf. Returns bytes written (>=0)
// or -1 on bad handle / wrong kind / missing rights / dev error.
//
// Dispatches by handle kind: KOBJ_SPOOR routes through the Dev `.write`
// vtable (Spoor.offset); KOBJ_SRV (a SrvConn client handle) routes
// through srvconn_client_write — a Twrite on the SrvConn's kernel-owned
// p9_client (using cn->client_offset). P5-corvus-srv-impl-b2.
s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf, u64 len) {
    if (!p || (!kbuf && len > 0))                    return -1;
    struct Handle *slot = sys_lookup_rw_handle(p, h, RIGHT_WRITE);
    if (!slot)                                       return -1;
    if (len == 0)                                    return 0;

    switch (slot->kind) {
    case KOBJ_SPOOR: {
        struct Spoor *c = (struct Spoor *)slot->obj;
        if (!c || !c->dev || !c->dev->write)         return -1;
        long n = c->dev->write(c, kbuf, (long)len, c->offset);
        if (n < 0)                                   return -1;
        c->offset += n;
        return (s64)n;
    }
    case KOBJ_SRV: {
        // sys_lookup_rw_handle validated SRV_CONN_MAGIC. F1 close
        // (P5-corvus-srv-impl audit): refresh the deadline before
        // each blocking op — same discipline as the read path.
        // P6-pouch-sockets (sub-chunk 12): byte-mode SrvConns route
        // through srvconn_client_send (raw chan_produce on c2s) —
        // no 9P Twrite framing. 9P-mode SrvConns keep the original
        // srvconn_client_write path. F5 close: ATOMIC_ACQUIRE pairs
        // srvconn_set_byte_mode's ATOMIC_RELEASE.
        struct SrvConn *cn = (struct SrvConn *)slot->obj;
        // R1 F3 close: refuse raw byte I/O on a kernel-attached SrvConn.
        // After SYS_ATTACH_9P_SRV wraps the conn, the c2s/s2c rings are
        // load-bearing for the kernel 9P client's Twalk/Tread/Twrite
        // stream. A concurrent userspace write would interleave bytes
        // into the c2s ring out-of-band w.r.t. the 9P session, corrupting
        // the wire. Reject upfront.
        if (srvconn_is_kernel_attached(cn))          return -1;
        srvconn_set_client_deadline(cn, timer_now_ns() + SRVCONN_OP_DEADLINE_NS);
        bool bm = __atomic_load_n(&cn->byte_mode, __ATOMIC_ACQUIRE);
        long n = bm
            ? srvconn_client_send(cn, kbuf, (long)len)
            : srvconn_client_write(cn, kbuf, (long)len);
        if (n < 0)                                   return -1;
        return (s64)n;
    }
    default:
        return -1;   // unreachable — sys_lookup_rw_handle filters
    }
}

// Inner — testable with kernel-side buf. Returns bytes read (>=0; 0
// on EOF) or -1 on bad handle / wrong kind / missing rights / dev error.
//
// Dispatches by handle kind: KOBJ_SPOOR routes through the Dev `.read`
// vtable (Spoor.offset); KOBJ_SRV routes through srvconn_client_read
// (a Tread on the SrvConn's kernel-owned p9_client). P5-corvus-srv-impl-b2.
s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len) {
    if (!p || (!kbuf && len > 0))                    return -1;
    struct Handle *slot = sys_lookup_rw_handle(p, h, RIGHT_READ);
    if (!slot)                                       return -1;
    if (len == 0)                                    return 0;

    switch (slot->kind) {
    case KOBJ_SPOOR: {
        struct Spoor *c = (struct Spoor *)slot->obj;
        if (!c || !c->dev || !c->dev->read)          return -1;
        long n = c->dev->read(c, kbuf, (long)len, c->offset);
        if (n < 0)                                   return -1;
        c->offset += n;
        return (s64)n;
    }
    case KOBJ_SRV: {
        // F1 close (P5-corvus-srv-impl audit): refresh the deadline
        // before each blocking op — same discipline as the handshake.
        // Without this `srvconn_client_recv` would tsleep with
        // deadline=0 and a hung corvus would wedge the caller.
        // P6-pouch-sockets (sub-chunk 12): byte-mode SrvConns route
        // through srvconn_client_recv (raw chan_consume on s2c) —
        // the same deadline machinery still bounds the blocking wait
        // (see srvconn_client_recv's tsleep against client_deadline_ns).
        // F5 close: ATOMIC_ACQUIRE pairs srvconn_set_byte_mode's RELEASE.
        struct SrvConn *cn = (struct SrvConn *)slot->obj;
        // R1 F3 close: refuse raw byte I/O on a kernel-attached SrvConn.
        // After SYS_ATTACH_9P_SRV the c2s/s2c rings carry the kernel
        // client's 9P stream; a userspace recv would drain Rwalk/Rread
        // bytes meant for the kernel client and corrupt the session.
        if (srvconn_is_kernel_attached(cn))          return -1;
        srvconn_set_client_deadline(cn, timer_now_ns() + SRVCONN_OP_DEADLINE_NS);
        bool bm = __atomic_load_n(&cn->byte_mode, __ATOMIC_ACQUIRE);
        long n = bm
            ? srvconn_client_recv(cn, kbuf, (long)len)
            : srvconn_client_read(cn, kbuf, (long)len);
        if (n < 0)                                   return -1;
        return (s64)n;
    }
    default:
        return -1;
    }
}

static s64 sys_write_handler(u64 hraw, u64 buf_va, u64 len) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -1;
    if (len > SYS_RW_MAX) len = SYS_RW_MAX;

    if (len == 0) {
        // Validate the handle even for zero-length writes (POSIX
        // discipline: bad fd should return -EBADF regardless of
        // len). Accepts both KOBJ_SPOOR + KOBJ_SRV (SrvConn) so a
        // zero-length write to a /srv client handle still validates.
        if (!sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_WRITE))
                                                     return -1;
        return 0;
    }

    // Bounce buffer on the kernel stack. 4 KiB cap → 4 KiB stack
    // frame; kernel test thread stack is 16 KiB, leaves headroom.
    u8 scratch[SYS_RW_MAX];
    for (u64 i = 0; i < len; i++) {
        if (uaccess_load_u8(buf_va + i, &scratch[i]) != 0) return -1;
    }
    return sys_write_for_proc(p, (hidx_t)hraw, scratch, len);
}

static s64 sys_read_handler(u64 hraw, u64 buf_va, u64 len) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -1;
    if (len > SYS_RW_MAX) len = SYS_RW_MAX;

    if (len == 0) {
        if (!sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_READ))
                                                     return -1;
        return 0;
    }

    u8 scratch[SYS_RW_MAX];
    s64 got = sys_read_for_proc(p, (hidx_t)hraw, scratch, len);
    if (got <= 0)                                    return got;

    // Copy what was read back to user-VA. Per-byte; on fault, return
    // -1 — partial bytes already in user-VA are not "uncopied" but
    // bytes consumed beyond the fault are LOST. Documented caveat.
    for (s64 i = 0; i < got; i++) {
        if (uaccess_store_u8(buf_va + (u64)i, scratch[i]) != 0) return -1;
    }
    return got;
}

// =============================================================================
// SYS_CLOSE / SYS_DUP — handle table operations (P5-fd-syscalls).
// =============================================================================
//
// SYS_CLOSE(fd) → 0 on success, -1 on invalid fd. Thin wrapper over
//                 handle_close. For KOBJ_SPOOR handles, the release
//                 path (wired at P5-fd-pipe) routes to spoor_clunk.
//
// SYS_DUP(oldfd, new_rights) → new fd (>=0) on success, -1 on bad
//                              oldfd / rights elevation / table-full.
//                              handle_dup's RightsCeiling check
//                              rejects new_rights that aren't a
//                              subset of oldfd's rights. For
//                              KOBJ_SPOOR the acquire path calls
//                              spoor_ref so each handle independently
//                              holds a reference.

static s64 sys_close_handler(u64 hraw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    return (s64)handle_close(p, (hidx_t)hraw);
}

// =============================================================================
// SYS_FSTAT / SYS_LSEEK — POSIX-shaped file-metadata + seek surfaces.
// =============================================================================
//
// P6-pouch-stratumd-boot sub-chunk 16b-gamma. Closes the boot mount path:
// stratumd's `stm_keyfile_load` opens /system.key, calls fstat() to learn
// the size (size-check gate), reads 8 bytes to identify the keyfile format,
// lseek(SEEK_SET, 0) to rewind, then reads the rest. Without these two
// syscalls, stratumd's keyfile_load fails at the first fstat / lseek and
// the system-pool mount never starts.
//
// SYS_FSTAT routes through dev->stat_native; only devramfs implements it
// at v1.0 (real file-metadata source). Other Devs leave the slot NULL and
// fstat returns -1, the graceful "no stat for this kind of object" answer.
// SrvConn (KOBJ_SRV) handles likewise reject — fstat on a socket isn't
// meaningful.
//
// SYS_LSEEK manipulates the per-Spoor `s64 offset` cursor that SYS_READ /
// SYS_WRITE advance per call. SEEK_END queries dev->stat_native for size;
// Devs without stat_native cannot service SEEK_END (returns -1). At v1.0
// no per-Spoor cursor lock — concurrent lseek/read/write on the same fd
// from different threads is unspecified (POSIX user serializes).

// Inner — kernel-side helper exposing the t_stat fill for a Spoor.
// Returns 0 on success (out populated), -1 on:
//   - c is NULL
//   - dev->stat_native is NULL (Dev does not support native stat)
//   - dev->stat_native returned an error
static int spoor_stat_native(struct Spoor *c, struct t_stat *out) {
    if (!c || !out)                                  return -1;
    if (!c->dev || !c->dev->stat_native)             return -1;
    return c->dev->stat_native(c, out);
}

static s64 sys_fstat_handler(u64 hraw, u64 stat_va) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // user-VA range validation. The full struct must lie within the
    // user-VA bound; sys_validate_user_buf rejects an out-of-range
    // base or end. Alignment is not required — the per-byte store
    // loop tolerates any alignment.
    if (!sys_validate_user_buf(stat_va, sizeof(struct t_stat))) return -1;

    // Rights gate: the handle must be KOBJ_SPOOR with RIGHT_READ.
    // sys_lookup_rw_handle filters KOBJ_SRV (SrvConns are not stat'able);
    // for KOBJ_SPOOR it also checks the rights mask.
    struct Handle *slot = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_READ);
    if (!slot)                                        return -1;
    if (slot->kind != KOBJ_SPOOR)                     return -1;

    struct Spoor *c = (struct Spoor *)slot->obj;

    // Fill a kernel-scratch t_stat from the Dev. Failing the Dev's
    // stat_native (NULL slot or error return) maps to -1 here.
    struct t_stat ks;
    if (spoor_stat_native(c, &ks) != 0)               return -1;

    // Copy out to user-VA. Per-byte uaccess_store_u8; on fault the
    // user-VA may have partially-written bytes (consistent with the
    // existing per-byte uaccess pattern in SYS_READ).
    const u8 *src = (const u8 *)&ks;
    for (u64 i = 0; i < sizeof(struct t_stat); i++) {
        if (uaccess_store_u8(stat_va + i, src[i]) != 0) return -1;
    }
    return 0;
}

static s64 sys_lseek_handler(u64 hraw, u64 offset_raw, u64 whence_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Whence range check before any handle work — cheap reject of
    // structurally invalid calls.
    if (whence_raw != T_SEEK_SET &&
        whence_raw != T_SEEK_CUR &&
        whence_raw != T_SEEK_END)                    return -1;

    // No rights mask: lseek manipulates the per-Spoor cursor, which is
    // metadata about an open file, not its content. sys_lookup_rw_handle
    // with rights=0 still validates kind + KOBJ_SRV rejection.
    struct Handle *slot = sys_lookup_rw_handle(p, (hidx_t)hraw, 0);
    if (!slot)                                        return -1;
    if (slot->kind != KOBJ_SPOOR)                     return -1;

    struct Spoor *c = (struct Spoor *)slot->obj;

    // R16b-gamma-mount-close audit F3 close: reject lseek on non-seekable
    // Devs. The presence of `dev->stat_native` is the seek-capability
    // indicator at v1.0: file-like Devs (devramfs) implement it; stream-
    // like Devs (devpipe, devnull, devzero, devnotes, devcons, devsrv)
    // leave it NULL. POSIX requires lseek(2) on a pipe to return -1
    // with ESPIPE; this check is the v1.0 equivalent (pouch's lseek
    // translates the kernel -1 to ESPIPE via the errno table). The
    // narrower `seekable` predicate is deferred to v1.x if any Dev ever
    // grows stat_native AND wants to refuse seek.
    if (c->dev->stat_native == NULL)                  return -1;

    s64 offset = (s64)offset_raw;
    s64 new_off;

    switch (whence_raw) {
    case T_SEEK_SET:
        if (offset < 0)                               return -1;
        new_off = offset;
        break;
    case T_SEEK_CUR: {
        s64 cur = c->offset;
        // Overflow / underflow check: cur + offset stays in s64 range
        // and is non-negative. INT64_MIN handled because offset is u64-
        // cast to s64; INT64_MIN would have appeared if the user passed
        // 0x8000000000000000, which still fails the "non-negative" check
        // after addition.
        if (offset > 0 && cur > INT64_MAX - offset)     return -1;
        if (offset < 0 && cur < INT64_MIN - offset)     return -1;
        new_off = cur + offset;
        if (new_off < 0)                                return -1;
        break;
    }
    case T_SEEK_END: {
        // Query size from dev->stat_native; Devs without native stat
        // cannot service SEEK_END.
        struct t_stat ks;
        if (spoor_stat_native(c, &ks) != 0)           return -1;
        s64 size = (s64)ks.size;
        // ks.size is a u64 cap'd by file system; cast to s64 may
        // overflow only for files >= 2^63 bytes (structurally impossible
        // at v1.0 — devramfs files are bounded by the cpio blob size
        // which is far below s64 max).
        if (size < 0)                                 return -1;
        if (offset > 0 && size > INT64_MAX - offset)  return -1;
        if (offset < 0 && size < INT64_MIN - offset)  return -1;
        new_off = size + offset;
        if (new_off < 0)                              return -1;
        break;
    }
    default:
        return -1;
    }

    c->offset = new_off;
    return new_off;
}

// =============================================================================
// SYS_ATTACH_9P — wrap a Spoor pair in a 9P client + return root fd
// (P5-attach-syscall).
// =============================================================================
//
// User-visible body of `attach_9p(tx_fd, rx_fd, aname, n_uname)` per
// ARCH §9.6.1. Composes:
//   - SYS_PIPE-style KOBJ_SPOOR fd inputs (tx + rx — caller picks
//     whether they're the same Spoor for duplex byte pipes, or two
//     distinct Spoors for half-duplex like the pipe(fd[2]) primitive).
//   - p9_spoor_transport adapter (binds the Spoor pair to the
//     transport_ops vtable).
//   - p9_attached_create (drives Tversion + Tattach handshake;
//     allocates the p9_client + recv_buf).
//   - p9_attached_root_spoor (constructs the dev9p root Spoor).
//   - dev9p_priv extension: stash the attached_owner + adapter so
//     spoor_clunk on the returned fd tears down the entire session.
//   - handle_alloc as KOBJ_SPOOR with RIGHT_READ|WRITE|TRANSFER.
//
// On any failure, ALL partial state is cleaned up (rollback).
//
// Rights gate: tx_fd needs RIGHT_WRITE; rx_fd needs RIGHT_READ.
//
// Aname validation: aname_va is a user-VA buffer of aname_len bytes
// (max SYS_ATTACH_ANAME_MAX = 256). Copied into kernel scratch via
// per-byte uaccess_load_u8. NULL with len=0 is allowed (empty aname).

// Default 9P handshake parameters for SYS_ATTACH_9P at v1.0. msize
// must match what dev9p / sys_read_handler scratch buffers can hold.
// 4 KiB matches PIPE_BUF_SIZE + SYS_RW_MAX; aligns with the design.
#define SYS_ATTACH_DEFAULT_MSIZE     4096u
#define SYS_ATTACH_DEFAULT_ROOT_FID  1u

// Map a p9_attached_create out-err to a syscall return (A-3c / M6). Surface a
// valid passthrough errno -- the [-4095, -2] range the pouch boundary-line
// translates to a userspace errno, e.g. -T_E_ACCES from a per-user-stratumd
// Tattach dataset-scope refusal -- otherwise the generic -1. Both SYS_ATTACH_9P
// and SYS_ATTACH_9P_SRV route their create-failure return through here so an
// out-of-scope attach is observably EACCES, not an undistinguished failure.
static inline s64 attach_err_to_ret(int aerr) {
    return (aerr <= -2 && aerr >= -4095) ? (s64)aerr : -1;
}

static s64 sys_attach_9p_handler(u64 tx_fd_raw, u64 rx_fd_raw,
                                 u64 aname_va, u64 aname_len, u64 n_uname) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Validate aname length cap.
    if (aname_len > SYS_ATTACH_ANAME_MAX)             return -1;
    // F239: n_uname is 9P2000.L's u32 numeric uid field; reject values
    // that would silently truncate to u32. Pre-fix the syscall did
    // `(u32)n_uname` blindly, masking high bits.
    if (n_uname > (u64)0xFFFFFFFFu)                   return -1;
    // Validate user-VA range when aname_len > 0; zero-length aname
    // is permitted (a zero-length attach name is legal per 9P2000.L).
    if (aname_len > 0 && !sys_validate_user_buf(aname_va, aname_len))
                                                     return -1;

    // Look up the transport handles. Take INDEPENDENT references so
    // userspace closing the original fds doesn't free Spoors out from
    // under the attach. The original fds keep their own ref via the
    // handle table; we add ours on top.
    struct Spoor *tx = sys_lookup_spoor(p, (hidx_t)tx_fd_raw, RIGHT_WRITE);
    if (!tx)                                         return -1;
    struct Spoor *rx = sys_lookup_spoor(p, (hidx_t)rx_fd_raw, RIGHT_READ);
    if (!rx)                                         return -1;
    spoor_ref(tx);
    if (rx != tx) spoor_ref(rx);

    // Copy aname into kernel scratch. SYS_ATTACH_ANAME_MAX byte stack
    // buffer; per-byte uaccess_load_u8 (same shape as SYS_WRITE).
    u8 aname_scratch[SYS_ATTACH_ANAME_MAX];
    for (u64 i = 0; i < aname_len; i++) {
        if (uaccess_load_u8(aname_va + i, &aname_scratch[i]) != 0) {
            spoor_unref(tx);
            if (rx != tx) spoor_unref(rx);
            return -1;
        }
    }

    // Allocate the adapter on the heap. Transport ownership transfers
    // into the p9_attached via p9_attached_install_transport below, so
    // the LAST p9_attached_unref (after every walked Spoor closes — F2)
    // is what kfree's the adapter and spoor_clunks the transport Spoors.
    struct p9_spoor_transport *adapter = kmalloc(sizeof(*adapter), KP_ZERO);
    if (!adapter) {
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        return -1;
    }
    // owns_spoors=false: dev9p (not the adapter) is the holder. The
    // attached's last unref releases tx/rx via spoor_clunk and kfree's
    // the adapter; the adapter's own close hook stays a no-op.
    if (p9_spoor_transport_init(adapter, tx, rx, false) != 0) {
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        kfree(adapter);
        return -1;
    }

    struct p9_transport_ops ops = p9_spoor_transport_ops(adapter);
    int aerr = 0;
    struct p9_attached *att = p9_attached_create(
        ops,
        SYS_ATTACH_DEFAULT_MSIZE,            // recv_cap (= msize at v1.0)
        SYS_ATTACH_DEFAULT_ROOT_FID,         // root_fid
        SYS_ATTACH_DEFAULT_MSIZE,            // msize (client proposal)
        NULL, 0,                             // uname (empty at v1.0; no-auth)
        aname_len > 0 ? aname_scratch : NULL, aname_len,
        // A-3 M4: assert the caller's kernel-stamped principal as n_uname.
        // The userspace-supplied n_uname is vestigial (validated above for
        // ABI hygiene, then superseded). Against Stratum this is a no-op
        // (it reconciles via SO_PEERCRED, ignoring n_uname); it is forward-
        // compat for a foreign 9P server with no SO_PEERCRED. SO_PEERCRED is
        // the live local channel (IDENTITY-DESIGN.md section 9.7 M1/M4).
        p->principal_id, &aerr);
    if (!att) {
        // p9_attached_create's failure leaves the adapter untouched
        // (the create's transport_ops.close runs on rollback, which is
        // a no-op for owns=false). We must still kfree the adapter +
        // release transport refs since they never transferred.
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        kfree(adapter);
        return attach_err_to_ret(aerr);
    }

    // F2: transfer adapter + transport Spoor refs into the attached.
    // From now on the attached owns them; failure-path rollbacks use
    // p9_attached_unref (which is the path that kfree's the adapter +
    // spoor_clunks the transports).
    if (p9_attached_install_transport(att, adapter, tx, rx) != 0) {
        // Shouldn't happen — first install on a fresh attached. If it
        // does, the attached doesn't own the adapter; rollback manually.
        p9_attached_unref(att);
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        kfree(adapter);
        return -1;
    }
    // From here on, FAILURE paths just unref `att`. The attached's
    // last-ref destroy handles adapter + transport cleanup.

    struct Spoor *root = p9_attached_root_spoor(att);
    if (!root) {
        p9_attached_unref(att);
        return -1;
    }

    // Patch the root Spoor's dev9p_priv with the attach-session
    // ownership pointer (F2: attached_owner is the refcounted holder;
    // adapter_to_free is gone — adapter is inside attached now). The
    // root contributes one p9_attached_ref; we own the construction
    // ref from p9_attached_create + transfer it here, so the bump+drop
    // sequence becomes: bump for root's hold, drop the construction ref.
    struct dev9p_priv *root_priv = (struct dev9p_priv *)root->aux;
    if (!root_priv || root_priv->magic != DEV9P_PRIV_MAGIC) {
        // Shouldn't happen — p9_attached_root_spoor's dev9p Spoor
        // always has a valid priv. Defensive rollback.
        spoor_clunk(root);
        p9_attached_unref(att);
        return -1;
    }
    root_priv->attached_owner = att;
    // The root holds its own ref now.
    p9_attached_ref(att);

    // Install root Spoor as a KOBJ_SPOOR handle. handle_alloc takes
    // ownership of root's ref (the one from spoor_alloc inside
    // p9_attached_root_spoor).
    rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, r, root);
    if (fd < 0) {
        // Roll back: clunking root triggers dev9p_close, which unrefs
        // root's hold. Then we drop the construction ref. Last unref
        // tears down the session (adapter + transports + client).
        spoor_clunk(root);
        p9_attached_unref(att);
        return -1;
    }
    // Drop the construction ref — root's hold + walked privs' future
    // holds are what keep the attached alive going forward.
    p9_attached_unref(att);
    return (s64)fd;
}

// =============================================================================
// SYS_ATTACH_9P_SRV — wrap a byte-mode SrvConn in a 9P client + return
// the root fd (P6-pouch-stratumd-boot 16c).
// =============================================================================
//
// Parallel to SYS_ATTACH_9P but the transport is a byte-mode KObj_Srv
// connection (the SrvConn from a SYS_SRV_CONNECT against a stratumd-style
// pouch-byte-mode service) instead of a Spoor pair. Composes:
//   - SrvConn handle lookup (KObj_Srv, rights R|W)
//   - byte-mode gate (the embedded kernel-owned p9_client of a 9P-mode
//     SrvConn owns the rings; a second p9_client would race / produce
//     wire corruption)
//   - kmalloc the p9_srvconn_transport adapter (takes 1 srvconn_ref)
//   - p9_attached_create (drives Tversion + Tattach; allocates the
//     p9_client + recv_buf)
//   - p9_attached_root_spoor (constructs the dev9p root Spoor)
//   - dev9p_priv extension: stash the attached_owner so spoor_clunk
//     on the returned fd tears down the entire attach session
//   - handle_alloc KOBJ_SPOOR with RIGHT_READ|WRITE|TRANSFER
//
// On any failure, ALL partial state is cleaned up (rollback). Mirrors
// SYS_ATTACH_9P's rollback discipline.
//
// Rights gate on the SrvConn: RIGHT_READ + RIGHT_WRITE. The kernel 9P
// client both writes Twalk/Tread/Twrite (RIGHT_WRITE) and reads
// Rwalk/Rread/Rwrite (RIGHT_READ).
//
// Aname validation: aname_va is a user-VA buffer of aname_len bytes
// (max SYS_ATTACH_ANAME_MAX = 256). Copied into kernel scratch via
// per-byte uaccess_load_u8. NULL with len=0 is allowed (empty aname).

static s64 sys_attach_9p_srv_handler(u64 srv_fd_raw, u64 aname_va,
                                       u64 aname_len, u64 n_uname) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Validate aname length cap.
    if (aname_len > SYS_ATTACH_ANAME_MAX)             return -1;
    // n_uname is 9P2000.L's u32 numeric uid field; reject values that
    // would silently truncate to u32. Mirrors SYS_ATTACH_9P F239 fix.
    if (n_uname > (u64)0xFFFFFFFFu)                   return -1;
    // Validate user-VA range when aname_len > 0; zero-length aname is
    // legal per 9P2000.L.
    if (aname_len > 0 && !sys_validate_user_buf(aname_va, aname_len))
                                                     return -1;

    // Look up the SrvConn handle. Rights: READ + WRITE (the kernel 9P
    // client both reads and writes through the byte rings). Same rights-
    // gate shape as SYS_ATTACH_9P's READ on rx + WRITE on tx.
    struct Handle *slot = handle_get(p, (hidx_t)srv_fd_raw);
    if (!slot)                                       return -1;
    if (slot->kind != KOBJ_SRV)                      return -1;
    if (!slot->obj)                                  return -1;
    if ((slot->rights & (RIGHT_READ | RIGHT_WRITE))
        != (RIGHT_READ | RIGHT_WRITE))                return -1;
    if (*(const u64 *)slot->obj != SRV_CONN_MAGIC)   return -1;

    struct SrvConn *cn = (struct SrvConn *)slot->obj;

    // Byte-mode gate. A 9P-mode SrvConn has an EMBEDDED kernel-owned
    // p9_client driving Tread/Twrite on a single client_fid (the
    // corvus-style verb stream). Wrapping its c2s/s2c rings with ANOTHER
    // p9_client via this adapter would interleave Tversion/Tattach/
    // Twalk frames with the embedded client's Tread/Twrite frames on
    // the SAME ring -- wire corruption.
    //
    // Atomic acquire pairs srvconn_set_byte_mode's RELEASE in
    // srv_conn_open_for_proc (mirror sys_srv_connect_for_proc's
    // discipline, F5 close).
    if (!__atomic_load_n(&cn->byte_mode, __ATOMIC_ACQUIRE))
                                                      return -1;

    // Copy aname into kernel scratch. Same shape as SYS_ATTACH_9P.
    u8 aname_scratch[SYS_ATTACH_ANAME_MAX];
    for (u64 i = 0; i < aname_len; i++) {
        if (uaccess_load_u8(aname_va + i, &aname_scratch[i]) != 0)
            return -1;
    }

    // Allocate the adapter on the heap. The adapter's init takes one
    // srvconn_ref; the eventual close (in p9_attached_destroy's
    // transport_ops.close call) drops it.
    struct p9_srvconn_transport *adapter = kmalloc(sizeof(*adapter), KP_ZERO);
    if (!adapter)                                    return -1;

    if (p9_srvconn_transport_init(adapter, cn) != 0) {
        kfree(adapter);
        return -1;
    }
    // From here adapter holds 1 srvconn_ref. Failure paths below must
    // either install the adapter into p9_attached (so its destroy
    // drops the ref via close) OR explicitly call the close + destroy
    // sequence + kfree.

    // R1 F4 close: hoist srvconn_set_kernel_attached HERE -- as early
    // as possible after the adapter has committed (1 srvconn_ref taken).
    // Pre-fix the setter ran at the end of the syscall (after handle_alloc
    // of the KOBJ_SPOOR root); a cross-CPU peer-Thread t_close on the
    // KObj_Srv handle in the same Proc could see kernel_attached=false
    // and tear the rings down DURING the handshake. Setting the flag here
    // closes that window: any subsequent handle_close on the userspace
    // KObj_Srv slot sees kernel_attached=true and skips srvconn_teardown.
    // Failure paths below still tear down cleanly because the adapter's
    // transport.close runs srvconn_teardown + srvconn_unref
    // unconditionally (idempotent on already-torn rings).
    srvconn_set_kernel_attached(cn);

    // R1 F1 close: set the handshake deadline BEFORE p9_attached_create
    // drives Tversion + Tattach. Pre-fix the syscall called the
    // handshake with `cn->client_deadline_ns == 0` -- which the
    // adapter's srvconn_client_recv interprets as "no deadline" (block
    // indefinitely on tsleep). A stratumd that was alive but hung
    // (mid-pthread-join, mallocng-asserted, etc.) would wedge joey
    // forever instead of failing cleanly. The peer sys_srv_connect_
    // handler sets the same deadline at line 3811; sys_attach_9p_srv_
    // handler now mirrors that discipline.
    srvconn_set_client_deadline(cn,
        timer_now_ns() + SRVCONN_HANDSHAKE_DEADLINE_NS);

    struct p9_transport_ops ops = p9_srvconn_transport_ops(adapter);
    int aerr = 0;
    struct p9_attached *att = p9_attached_create(
        ops,
        SYS_ATTACH_DEFAULT_MSIZE,            // recv_cap (= msize at v1.0)
        SYS_ATTACH_DEFAULT_ROOT_FID,         // root_fid
        SYS_ATTACH_DEFAULT_MSIZE,            // msize (client proposal)
        NULL, 0,                             // uname (empty; no-auth v1.0)
        aname_len > 0 ? aname_scratch : NULL, aname_len,
        // A-3 M4: assert the caller's kernel-stamped principal as n_uname
        // (vestigial userspace arg superseded; SO_PEERCRED is the live local
        // channel for stratumd). See section 9.7 + the sys_attach_9p_handler twin.
        p->principal_id, &aerr);
    if (!att) {
        // Handshake failed (server unresponsive, deadline, Rlerror, OOM).
        // The adapter still holds its srvconn_ref; release it manually
        // since we did not transfer ownership.
        struct p9_transport_ops cops = p9_srvconn_transport_ops(adapter);
        if (cops.close) (void)cops.close(cops.ctx);
        p9_srvconn_transport_destroy(adapter);
        kfree(adapter);
        // A-3c/M6: the per-user stratumd dataset-scope refusal arrives as a
        // Tattach Rlerror(EACCES); surface -T_E_ACCES so the attacher observes
        // EACCES, not a bare -1. Other handshake failures map likewise.
        return attach_err_to_ret(aerr);
    }

    // Transfer adapter ownership into the attached. Mirrors SYS_ATTACH_
    // 9P's F2 install_transport flow, but with a SrvConn-shaped adapter:
    // we use install_transport with tx == rx == NULL (no transport-Spoor
    // refs to manage -- the SrvConn lifetime is owned by the adapter's
    // own srvconn_ref). install_transport now owns the adapter; its
    // destroy path will call transport_ops.close (-> srvconn_unref) +
    // kfree(adapter).
    //
    // Note: p9_attached_install_transport's tx/rx parameters are
    // Spoor *; passing NULL is documented as the "test-loopback" path
    // (no transport-Spoor refs to drop). Re-using it for our SrvConn-
    // backed transport works because the adapter type carries its own
    // srvconn_ref via its close op -- the Spoor-pair plumbing is
    // unused. A future refactor (v1.x) could give p9_attached an
    // adapter-only install path; at v1.0 the NULL Spoor pair is the
    // documented safe combination.
    if (p9_attached_install_transport(att, (struct p9_spoor_transport *)adapter,
                                       NULL, NULL) != 0) {
        // Shouldn't happen — first install on a fresh attached. Defensive.
        //
        // R2 F2R2 close: clean up the adapter manually. p9_attached_unref
        // triggers destroy -> p9_client_close -> transport.ops.close
        // (which drops the adapter's srvconn_ref via srvconn_unref).
        // But attached_destroy_inner's `if (a->adapter)` block is SKIPPED
        // because install_transport never set a->adapter -- so the
        // kmalloc'd adapter struct itself leaks unless we kfree it here.
        // Call destroy first (clobbers magic; mirrors the discipline in
        // attached_destroy_inner's adapter-installed path).
        p9_attached_unref(att);
        p9_srvconn_transport_destroy(adapter);
        kfree(adapter);
        return -1;
    }
    // From here failure paths just unref `att`. The attached's last-ref
    // destroy handles adapter + srvconn cleanup via the close vtable.

    struct Spoor *root = p9_attached_root_spoor(att);
    if (!root) {
        p9_attached_unref(att);
        return -1;
    }

    // Patch the root Spoor's dev9p_priv with the attach-session
    // ownership pointer. Mirrors SYS_ATTACH_9P's pattern: bump for
    // root's hold, drop the construction ref.
    struct dev9p_priv *root_priv = (struct dev9p_priv *)root->aux;
    if (!root_priv || root_priv->magic != DEV9P_PRIV_MAGIC) {
        spoor_clunk(root);
        p9_attached_unref(att);
        return -1;
    }
    root_priv->attached_owner = att;
    p9_attached_ref(att);

    // Install the root Spoor as a KOBJ_SPOOR handle.
    rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, r, root);
    if (fd < 0) {
        // Roll back: clunk the root (dev9p_close unrefs the attached's
        // root contribution + the construction ref drops below).
        spoor_clunk(root);
        p9_attached_unref(att);
        return -1;
    }

    // Drop the construction ref. (kernel_attached was set EARLIER --
    // right after p9_srvconn_transport_init succeeded; see R1 F4 fix.)
    p9_attached_unref(att);
    return (s64)fd;
}

// =============================================================================
// SYS_PIVOT_ROOT — long-running-Proc root_spoor swap (P6-pouch-stratumd-
// boot 16c).
// =============================================================================
//
// Thin SVC wrapper over territory_pivot_root. Unlike SYS_CHROOT (the
// initial-chroot primitive joey + kproc use at boot), SYS_PIVOT_ROOT
// REQUIRES the caller's Territory to have a current root_spoor. Joey
// calls this LAST in its bringup, swapping its devramfs root for
// stratumd's mounted FS root.
//
// Audit-trigger: touches `kernel/territory.c` (CLAUDE.md §25.4 — Territory)
// via territory_pivot_root. Adds no new mount-table edge (no I-3 / I-1
// implications). MountRefcountConsistency holds via the matched bump +
// drop in territory_pivot_root.

static s64 sys_pivot_root_handler(u64 new_root_fd_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!p->territory)                               return -1;

    // RIGHT_READ on the new root: same rationale as SYS_CHROOT -- pivot
    // serves as a walk source post-pivot. RIGHT_WRITE not required:
    // pivot binds a name to a Spoor without inheriting per-handle
    // rights; subsequent SYS_WALK_OPEN(FROM_ROOT) re-establishes rights
    // from the freshly-cloned Spoor's Dev. Mount-style operations that
    // create new edges in the namespace need W; pivot only swaps an
    // existing R-rights name binding (R1 F10 close).
    struct Spoor *source = sys_lookup_spoor(p, (hidx_t)new_root_fd_raw, RIGHT_READ);
    if (!source)                                     return -1;

    // territory_pivot_root handles: NULL-source rejection, no-current-
    // root rejection (the pivot pre-condition), idempotent same-pointer,
    // prior-root displacement via spoor_clunk, spoor_ref of the new.
    if (territory_pivot_root(p->territory, source) != 0) return -1;
    return 0;
}

// =============================================================================
// SYS_WALK_OPEN — single-component walk-and-open through a Spoor's Dev
// vtable (P5-stratumd-stub-bringup-e1). Plan-9 namec() in miniature:
// spoor_clone + dev->walk + dev->open + handle_alloc, composed atomically.
//
// The v1.0 minimum primitive to reach a file under an attached / mounted
// 9P root before the full open(name, mode) namec walker lands. Single
// component only (no '/' splitting, no '.' / '..') — keeps the path-
// validation surface tiny + the audit envelope narrow. Multi-component
// + path-traversal handling defer to the production open() chunk.
//
// Dev-agnostic: any Dev that implements both .walk and .open works. v1.0
// callers exercise dev9p (the attach_9p root) + transitively any walked
// dev9p subtree. devramfs Spoors are mostly directory-walked from the
// kernel side at v1.0, but the syscall does not gatekeep on `dc`.
// =============================================================================

static s64 sys_walk_open_handler(u64 spoor_fd_raw, u64 name_va,
                                  u64 name_len_raw, u64 omode_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Validate name length cap. name_len_raw == 0 is rejected: a zero-
    // length name is a clone-walk (nname=0 in the 9P sense), which has
    // no userspace use case for an opened fd at v1.0 — the attach root
    // is already opened.
    if (name_len_raw == 0)                            return -1;
    if (name_len_raw > SYS_WALK_OPEN_NAME_MAX)        return -1;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;

    // Validate omode bit set. Rejecting unknown bits lets future bits be
    // added without ambiguity (an old kernel rejects a new bit; a new
    // kernel accepts both old + new bits).
    if (omode_raw & ~(u64)SYS_WALK_OPEN_OMODE_VALID)  return -1;

    // Resolve the source Spoor. Two source paths share the rest of the
    // handler:
    //
    //   (1) handle-based (the default): spoor_fd_raw names a KOBJ_SPOOR
    //       handle the caller holds; RIGHT_READ is the gate for that
    //       handle. This is the e1 path used by /stub-walk-probe with
    //       its attach_fd, by /stub-fs-probe, etc.
    //
    //   (2) FROM_ROOT sentinel (the e2 extension): spoor_fd_raw ==
    //       SYS_WALK_OPEN_FROM_ROOT means "walk from my territory's
    //       pivoted root_spoor". No handle lookup; the territory's
    //       own ref keeps the Spoor alive across the syscall. Failure
    //       mode: caller has not called SYS_CHROOT yet (root_spoor ==
    //       NULL) → -1.
    //
    // Both paths converge on src; src is NOT spoor_ref'd here — the
    // syscall's source ownership stays with the caller (handle table or
    // Territory). spoor_clone(src) below is what mints the new Spoor for
    // the result fd.
    struct Spoor *src;
    if (spoor_fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
        if (!p->territory)                            return -1;
        src = p->territory->root_spoor;
        if (!src)                                     return -1;
    } else {
        src = sys_lookup_spoor(p, (hidx_t)spoor_fd_raw, RIGHT_READ);
        if (!src)                                     return -1;
    }
    if (!src->dev || !src->dev->walk || !src->dev->open) return -1;

    // A-2d (IDENTITY-DESIGN.md 3.7.1): search (X) permission on the source
    // directory before walking into it. Gated on the Dev's perm_enforced flag
    // (dev9p deferred to A-3; devramfs live). devramfs dirs are 0555, so the
    // PRINCIPAL_SYSTEM owner traverses while a non-system principal needs
    // other-x. fail-closed if the Dev cannot vouch for the metadata. This is
    // additive to the handle-RIGHT gate above (capability axis); both must hold.
    if (src->dev->perm_enforced) {
        struct t_stat src_st;
        if (spoor_stat_native(src, &src_st) != 0)        return -1;
        if (perm_check(p, &src_st, PERM_X) != 0)         return -1;
    }

    // Copy the name into kernel scratch + validate component shape.
    // Reject '/' (multi-component path — defer to production open()),
    // '\0' (truncation attack), and the special entries "." (no-op) +
    // ".." (parent traversal — only meaningful with a multi-component
    // path resolver). The component-shape check is intentionally strict
    // at v1.0; if a caller needs '.' it can pass a clone-walk later.
    //
    // F1 close (P5-stratumd-stub-bringup-e1+e2 audit): the scratch is
    // SYS_WALK_OPEN_NAME_MAX + 1 bytes so the NUL terminator below can
    // ALWAYS be written, even when name_len_raw == SYS_WALK_OPEN_NAME_MAX.
    // The Dev `walk` vtable (`<thylacine/dev.h>`) signature is
    // `(*walk)(c, nc, names, nname)` — there is NO length array; the
    // dev9p_walk impl scans for '\0' to discover each name's length
    // (kernel/dev9p.c, the `while (s[l] != '\0') l++;` loop). NUL
    // termination is REQUIRED, not "defense-in-depth": without it,
    // a max-length name causes dev9p_walk to walk past the scratch's
    // end into adjacent kernel-stack bytes and ship them on the wire,
    // leaking saved registers / return addresses / KASLR slide.
    char name_scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(name_va + i, &b) != 0)    return -1;
        if (b == '/' || b == '\0')                    return -1;
        name_scratch[i] = (char)b;
    }
    if (name_len_raw == 1 && name_scratch[0] == '.')  return -1;
    if (name_len_raw == 2 && name_scratch[0] == '.' &&
                              name_scratch[1] == '.') return -1;
    // Unconditional NUL terminator — REQUIRED for dev9p_walk's strlen scan.
    name_scratch[name_len_raw] = '\0';

    // Clone the source Spoor — gives us an independent cursor whose aux
    // dev->walk will replace with a freshly-allocated priv (carrying the
    // new fid). The clone starts at ref=1; spoor_clunk on failure runs
    // dev->close (clunks the fid if walk had progressed) + drops the ref.
    struct Spoor *nc = spoor_clone(src);
    if (!nc)                                          return -1;

    // Issue the walk. Pack the single name + length into one-element
    // arrays for the dev vtable's nname-style signature. dev9p_walk
    // allocates a fresh fid + drives p9_client_walk + replaces nc->aux
    // with new_priv (fid_owned=true); on failure it clunks the fid + frees
    // the Walkqid carrier itself.
    const char *names[1] = { name_scratch };
    struct Walkqid *w = src->dev->walk(src, nc, names, 1);
    if (!w) {
        // Walk failed — nc->aux is still the shallow copy of src->aux
        // (dev9p_walk replaces aux only on success). Calling dev->close
        // on nc would clunk src's fid through the shared aux — wrong.
        // Bypass close: detach aux + spoor_unref. The walkqid_free is
        // dev9p_walk's responsibility on its own failure path (it frees
        // the Walkqid before returning NULL).
        nc->aux = NULL;
        spoor_unref(nc);
        return -1;
    }
    // F4 close (P5-stratumd-stub-bringup audit): the Dev `walk` vtable
    // is documented as permissive ("Either reuses nc OR ignores it and
    // returns a fresh Spoor"; <thylacine/dev.h>). This handler depends
    // on the reuse-nc shape — it calls `nc->dev->open(nc, ...)` and
    // `handle_alloc(p, KOBJ_SPOOR, r, nc)`. A Dev whose walk allocates
    // its own Spoor (e.g., devramfs_walk's `cur = spoor_clone(c)` shape)
    // would cause the handler to: (a) open the unwalked nc (wrong qid);
    // (b) leak w->spoor with no caller knowing to free it. Reject any
    // Dev whose walk violates the reuse-nc convention. At v1.0 only
    // dev9p is user-reachable here, so this check is defense-in-depth
    // against a future Dev that exposes user-walkable Spoors with a
    // self-cloning walk impl.
    if (w->spoor != nc) {
        walkqid_free(w);
        nc->aux = NULL;
        spoor_unref(nc);
        return -1;
    }
    // F-16b-gamma close: reject partial walks. A Dev whose walk reports
    // nqid < nname (here always 1 for the single-component v1.0
    // contract) means the requested name did not resolve. dev9p_walk
    // returns NULL outright on Rerror; devramfs_walk (P6-pouch-stratumd-
    // boot 16b-gamma) returns a wq with nqid=0 on miss. Both paths must
    // produce -1 here. Without this check, a walk-miss returned a fd
    // bound to the SOURCE Spoor's qid (still the source's pre-walk
    // value, which for FROM_ROOT is the directory root) — open() would
    // open root as if the named file existed.
    if (w->nqid != 1) {
        walkqid_free(w);
        spoor_clunk(nc);
        return -1;
    }
    // dev9p's walkqid carrier has nc as its spoor; we own + free it
    // now that we've consumed the walk result (we don't need the qids
    // returned in w->qid[] — the next open() will refresh nc->qid).
    walkqid_free(w);

    // FS-delta (IDENTITY-DESIGN.md §9.4): SYS_WALK_OPEN_OPATH skips the
    // open. nc is walked (dev9p_walk set its fid + qid) but NOT Tlopen'd,
    // yielding a non-opened, walkable handle -- the valid base for
    // creating/walking children + a valid chroot target. 9P forbids Twalk
    // from an opened fid, so a normally-opened handle cannot serve that
    // role. The access bits are irrelevant for an O_PATH handle.
    if (!(omode_raw & SYS_WALK_OPEN_OPATH)) {
        // A-2d: R and/or W permission on the walked target per the open mode
        // (OREAD->R, OWRITE->W, ORDWR->R|W, OEXEC->X; OTRUNC adds W). O_PATH is
        // exempt from this gate (a walk-only handle has no access semantics --
        // FS-delta 9.4) but NOT from the src X-search above. nc->dev == src->dev
        // so perm_enforced matches; re-checked on nc for clarity.
        if (nc->dev->perm_enforced) {
            struct t_stat nc_st;
            if (spoor_stat_native(nc, &nc_st) != 0)  { spoor_clunk(nc); return -1; }
            if (perm_check(p, &nc_st, perm_want_for_omode((u32)omode_raw)) != 0) {
                spoor_clunk(nc);
                return -1;
            }
        }
        // Issue the open. dev9p_open returns nc itself (state mutated:
        // COPEN flag set, mode + offset reset, qid refreshed from Rlopen).
        // On failure nc still has its own walk-allocated fid; spoor_clunk
        // runs dev->close → p9_client_clunk on that fid + frees the priv.
        if (!nc->dev->open(nc, (int)omode_raw)) {
            spoor_clunk(nc);
            return -1;
        }
    }

    // Install the now-opened nc in the caller's handle table. The
    // handle_alloc takes ownership of the ref from spoor_clone; on full-table
    // failure we spoor_clunk (running dev->close to clunk the fid).
    //
    // A-3b (closes A-2d audit F1): the handle rights are DERIVED FROM omode
    // (rights_for_omode) so the capability axis cannot exceed the access
    // perm_check validated above. Before A-3b a hardcoded R|W|TRANSFER was
    // installed (the "9P-server-mediated" v1.0 policy, valid only while
    // dev9p.perm_enforced was false); once dev9p enforces, that envelope
    // outran the checked omode -- an OREAD/OEXEC open of an r-- / --x file
    // would yield a writable/readable handle perm_check never validated
    // (SYS_READ / SYS_WRITE re-check only the RIGHT, by the open-time-snapshot
    // design). A future walkable Dev inherits this derivation for free.
    //
    // F5 (A-1.7 audit): a T_OPATH (non-opened) handle is a directory
    // navigation / capability base, not a byte-I/O channel -- born R|W (it
    // must be a valid create/walk target for a confined storage-root cap) with
    // NO RIGHT_TRANSFER (it cannot be 9P-transferred once that surface lands).
    // This is the one case NOT derived from omode & 3.
    rights_t r;
    if (omode_raw & SYS_WALK_OPEN_OPATH) {
        r = RIGHT_READ | RIGHT_WRITE;
    } else {
        r = rights_for_omode((u32)omode_raw) | RIGHT_TRANSFER;
    }
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, r, nc);
    if (fd < 0) {
        spoor_clunk(nc);
        return -1;
    }
    return (s64)fd;
}

// =============================================================================
// SYS_WALK_CREATE — the create-then-open sibling of SYS_WALK_OPEN
// (convergence-detour FS-mutation foundation; IDENTITY-DESIGN.md §9.2).
//
// Creates the single component `name` inside the directory `parent_fd` and
// returns a NEW opened KOBJ_SPOOR fd referring to the created object (file or,
// when perm carries DMDIR, a directory). The mechanism:
//   1. resolve the parent dir Spoor (RIGHT_WRITE — create mutates the dir).
//   2. spoor_clone(parent) -> nc (shallow aux = parent's fid).
//   3. CLONE-walk nc (dev->walk with nname=0) so nc holds its OWN fid still
//      pointing at the parent dir (so create doesn't mutate the parent's fid).
//   4. nc->dev->create(nc, name, omode, perm, primary_gid) — does Tlcreate
//      (file) or Tmkdir+walk+lopen (dir) on nc's fid, leaving nc opened on the
//      new object. Returns nc on success, NULL on failure.
//   5. install nc in the handle table (R|W|TRANSFER, matching SYS_WALK_OPEN).
//
// The created object's group is the CALLER's primary_gid (A-1a identity on the
// Proc), carried into the 9P gid field. Per-file rwx ENFORCEMENT is A-2d; this
// is the create MECHANISM (I-22 holds — nothing enforces rwx yet to bypass).
// =============================================================================

static s64 sys_walk_create_handler(u64 parent_fd_raw, u64 name_va,
                                     u64 name_len_raw, u64 omode_raw,
                                     u64 perm_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Name length cap (same shape as SYS_WALK_OPEN; single-component only).
    if (name_len_raw == 0)                            return -1;
    if (name_len_raw > SYS_WALK_OPEN_NAME_MAX)        return -1;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;

    // omode bit validation (reject unknown bits; forward-compat).
    if (omode_raw & ~(u64)SYS_WALK_OPEN_OMODE_VALID)  return -1;

    // perm bit validation: only the low-9 mode bits + DMDIR are permitted at
    // v1.0. Any other DM* bit (DMAPPEND / DMEXCL / DMTMP / ...) -> -1, so a
    // future bit cannot be silently dropped. Also reject the full 64-bit raw
    // having bits above 32 (perm is a u32 ABI field).
    if (perm_raw & ~(u64)SYS_WALK_CREATE_PERM_VALID)  return -1;
    u32 perm = (u32)perm_raw;

    // Resolve the parent directory Spoor. RIGHT_WRITE is the gate: create
    // mutates the directory's contents. (SYS_WALK_OPEN uses RIGHT_READ; create
    // is the write-side op.) The FROM_ROOT sentinel walks from the caller's
    // pivoted Territory root, same as SYS_WALK_OPEN.
    struct Spoor *src;
    if (parent_fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
        if (!p->territory)                            return -1;
        src = p->territory->root_spoor;
        if (!src)                                     return -1;
    } else {
        src = sys_lookup_spoor(p, (hidx_t)parent_fd_raw, RIGHT_WRITE);
        if (!src)                                     return -1;
    }
    if (!src->dev || !src->dev->walk || !src->dev->create) return -1;

    // A-2d: write + search (W|X) permission on the parent directory before
    // creating in it. Gated on perm_enforced (dev9p deferred to A-3). devramfs
    // is read-only (its .create stub returns NULL) so this is effectively dead
    // for devramfs, but correct: a non-system principal lacks other-w on a 0755
    // dir and is denied here before the create attempt. fail-closed on no stat.
    if (src->dev->perm_enforced) {
        struct t_stat parent_st;
        if (spoor_stat_native(src, &parent_st) != 0)          return -1;
        if (perm_check(p, &parent_st, PERM_W | PERM_X) != 0)  return -1;
    }

    // Copy + validate the component name (same strict shape as SYS_WALK_OPEN:
    // reject '/' '\0' "." ".."; NUL-terminate for dev9p's strlen scan).
    char name_scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(name_va + i, &b) != 0)    return -1;
        if (b == '/' || b == '\0')                    return -1;
        name_scratch[i] = (char)b;
    }
    if (name_len_raw == 1 && name_scratch[0] == '.')  return -1;
    if (name_len_raw == 2 && name_scratch[0] == '.' &&
                              name_scratch[1] == '.') return -1;
    name_scratch[name_len_raw] = '\0';

    // Clone the parent, then CLONE-walk so nc carries its own fid at the
    // parent dir (a 0-component walk). create then mutates nc's fid into the
    // new object without touching the parent's fid.
    //
    // Cross-Dev clone-walk safety (this is the first userspace path to call a
    // Dev walk with nname==0; SYS_WALK_OPEN always passes nname>=1). Three
    // safe shapes (F1 audit -- devramfs is in the SECOND bucket, not the
    // reject bucket):
    //   (a) leaf Devs (cons/null/zero/full/random/pipe/notes/none) return NULL
    //       -> the walk-fail path below.
    //   (b) Devs that REUSE nc on a clone (devcap/devsrv/devramfs return
    //       w->spoor == nc, nqid==0) -> the create call proceeds but their
    //       create stub returns NULL, and their clone carries aux==NULL (or a
    //       no-op close), so the eventual spoor_clunk(nc) is harmless.
    //   (c) self-cloning dir Devs (devproc/devctl IGNORE nc and clone
    //       internally) return w->spoor != nc -> the reject path below (which
    //       now clunks the leaked clone, F2).
    // Only dev9p replaces nc->aux with a real fresh fid -- the only Dev whose
    // create actually creates at v1.0.
    struct Spoor *nc = spoor_clone(src);
    if (!nc)                                          return -1;

    struct Walkqid *w = src->dev->walk(src, nc, NULL, 0);
    if (!w) {
        // Clone-walk failed; nc->aux is still the shallow copy of src->aux
        // (dev9p_walk replaces aux only on success). Detach + unref without
        // running close (close would clunk src's fid through the shared aux).
        nc->aux = NULL;
        spoor_unref(nc);
        return -1;
    }
    // Defense-in-depth: the reuse-nc contract (same rationale as
    // SYS_WALK_OPEN's F4 close). A clone-walk returns nqid==0, so we do NOT
    // apply the nqid==1 partial-walk check here.
    if (w->spoor != nc) {
        // F2 audit: a self-cloning Dev (devproc/devctl) returned its OWN fresh
        // Spoor (ref=1) instead of reusing nc -- clunk it so it doesn't leak.
        // (Unreachable at v1.0: no writable self-cloning Dev is user-exposed;
        // correct for when one is.)
        if (w->spoor) spoor_clunk(w->spoor);
        walkqid_free(w);
        nc->aux = NULL;
        spoor_unref(nc);
        return -1;
    }
    walkqid_free(w);

    // Create + open the new object. dev->create returns nc (opened) on
    // success or NULL on failure; on NULL nc still owns its walked fid, so
    // spoor_clunk runs dev->close -> clunks it.
    struct Spoor *opened = nc->dev->create(nc, name_scratch, (int)omode_raw,
                                            perm, p->primary_gid);
    if (!opened) {
        spoor_clunk(nc);
        return -1;
    }

    // A-3 audit F1 (the create leg, closed in lockstep with the SYS_WALK_OPEN
    // leg): derive the handle rights from omode (rights_for_omode) so the
    // capability axis cannot exceed the access. A freshly-created file is
    // normally OWRITE/ORDWR -> RIGHT_WRITE; readers re-open OREAD. The mkdir
    // path (mkdir_or_open) creates with OREAD then CLOSES the handle and walks
    // T_OPATH for the navigation base, so the OREAD->RIGHT_READ create handle
    // is never used as a create base. Normally-opened -> RIGHT_TRANSFER.
    rights_t r = rights_for_omode((u32)omode_raw) | RIGHT_TRANSFER;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, r, nc);
    if (fd < 0) {
        spoor_clunk(nc);
        return -1;
    }
    return (s64)fd;
}

// =============================================================================
// SYS_FSYNC — durability barrier (FS-mutation foundation; IDENTITY-DESIGN.md
// §9.2). RIGHT_WRITE (fsync is the write-side flush). NULL .fsync slot -> -1.
// =============================================================================

static s64 sys_fsync_handler(u64 fd_raw, u64 datasync_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    struct Spoor *c = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_WRITE);
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->fsync)                   return -1;

    // Normalize datasync to 0/1 (any non-zero is "data only").
    u32 datasync = (datasync_raw != 0) ? 1u : 0u;
    return c->dev->fsync(c, datasync) == 0 ? 0 : -1;
}

// =============================================================================
// SYS_READDIR — directory enumeration (FS-mutation foundation; §9.2).
// RIGHT_READ on a directory Spoor. Returns the next run of 9P2000.L dirents
// into the user buffer, advancing the Spoor's offset to the last entry's
// Treaddir cookie. 0 bytes == end-of-directory. NULL .readdir slot -> -1.
//
// 9P2000.L dirent layout (per entry): qid(13) + offset(8 LE) + type(1) +
// name_len(2 LE) + name(name_len). The Treaddir "offset" is a RESUME COOKIE
// (the offset field of the last returned entry), NOT a byte position -- so the
// handler parses the returned run for the last entry's cookie and stores THAT
// in c->offset for the next call (mirrors Linux v9fs).
// =============================================================================

static s64 sys_readdir_handler(u64 fd_raw, u64 buf_va, u64 buf_len_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if (buf_len_raw == 0 || buf_len_raw > SYS_RW_MAX) return -1;
    if (!sys_validate_user_buf(buf_va, buf_len_raw))  return -1;

    struct Spoor *c = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_READ);
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->readdir)                 return -1;

    u8 scratch[SYS_RW_MAX];
    long got = c->dev->readdir(c, scratch, (long)buf_len_raw, c->offset);
    if (got < 0)                                     return -1;
    if (got == 0)                                    return 0;   // EOD; offset unchanged

    // Walk the returned dirents (bounded by `got`) to find the last complete
    // entry's offset cookie. The minimum entry is 24 bytes (qid+offset+type+
    // name_len) + a 0-length name. A run with no complete entry is a malformed
    // stream -> -1 (also prevents a userspace re-read spin on a non-advancing
    // offset).
    long pos = 0;
    u64 last_cookie = 0;
    bool advanced = false;
    while (pos + 24 <= got) {
        u64 cookie = 0;
        for (int i = 0; i < 8; i++)
            cookie |= (u64)scratch[pos + 13 + i] << (8 * i);
        u32 nlen = (u32)scratch[pos + 22] | ((u32)scratch[pos + 23] << 8);
        long entry = 24 + (long)nlen;
        if (pos + entry > got)                       break;       // truncated trailing entry
        last_cookie = cookie;
        advanced = true;
        pos += entry;
    }
    if (!advanced)                                   return -1;   // malformed run

    // Copy the dirent bytes to user-VA FIRST, THEN advance the Spoor offset
    // (F3 audit). If a uaccess store faults, we return -1 with the offset
    // UNCHANGED, so the caller's retry re-fetches the same run rather than
    // silently skipping the entries it never received.
    for (long i = 0; i < got; i++) {
        if (uaccess_store_u8(buf_va + (u64)i, scratch[i]) != 0) return -1;
    }
    c->offset = (s64)last_cookie;
    return got;
}

// =============================================================================
// SYS_RENAME + SYS_UNLINK — rename/move + remove (FS-mutation foundation
// FS-gamma; IDENTITY-DESIGN.md §9.3). Unlike SYS_WALK_CREATE, the 9P Trenameat
// / Tunlinkat verbs operate on the dirfid(s) BY NAME without transitioning
// them, so these handlers run the Dev op DIRECTLY on the looked-up dir Spoor(s)
// -- no clone-walk (mirrors SYS_FSYNC / SYS_READDIR). They are the atomic-swap
// substrate for A-1b's corvus identity-DB persistence + the A-2 coreutils.
// =============================================================================

// Resolve a directory fd argument for a mutation op: the FROM_ROOT sentinel ->
// the caller's Territory root_spoor (may be NULL -> caller rejects); otherwise
// a KOBJ_SPOOR handle gated on RIGHT_WRITE (the directory is mutated).
static struct Spoor *sys_resolve_dir_wr(struct Proc *p, u64 fd_raw) {
    if (fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
        if (!p->territory)                            return NULL;
        return p->territory->root_spoor;
    }
    return sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_WRITE);
}

// Copy + validate a single-component name from user-VA into `scratch` (NUL-
// terminated), with the same strict shape SYS_WALK_CREATE uses: reject empty /
// over-length / '/' / '\0' / "." / "..". `scratch` must be at least
// SYS_WALK_OPEN_NAME_MAX + 1 bytes. Returns 0 on success, -1 on any violation.
static int sys_copy_component(u64 name_va, u64 name_len, char *scratch) {
    if (name_len == 0)                                return -1;
    if (name_len > SYS_WALK_OPEN_NAME_MAX)            return -1;
    if (!sys_validate_user_buf(name_va, name_len))    return -1;
    for (u64 i = 0; i < name_len; i++) {
        u8 b;
        if (uaccess_load_u8(name_va + i, &b) != 0)    return -1;
        if (b == '/' || b == '\0')                    return -1;
        scratch[i] = (char)b;
    }
    if (name_len == 1 && scratch[0] == '.')           return -1;
    if (name_len == 2 && scratch[0] == '.' &&
                          scratch[1] == '.')           return -1;
    scratch[name_len] = '\0';
    return 0;
}

static s64 sys_rename_handler(u64 olddir_fd_raw, u64 oldname_va, u64 oldname_len_raw,
                               u64 newdir_fd_raw, u64 newname_va, u64 newname_len_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Validate + copy both names BEFORE resolving fds (cheap rejects first;
    // matches SYS_WALK_CREATE's name-validate-then-resolve order).
    char old_scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    char new_scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    if (sys_copy_component(oldname_va, oldname_len_raw, old_scratch) != 0) return -1;
    if (sys_copy_component(newname_va, newname_len_raw, new_scratch) != 0) return -1;

    struct Spoor *od = sys_resolve_dir_wr(p, olddir_fd_raw);
    if (!od)                                          return -1;
    struct Spoor *nd = sys_resolve_dir_wr(p, newdir_fd_raw);
    if (!nd)                                          return -1;
    if (!od->dev || !od->dev->rename)                 return -1;
    // Two-cursor + cross-Dev invariant: a 9P renameat is within ONE server, so
    // both directories MUST be on the same Dev (dev9p_rename adds the same-
    // session guard). Rejected here before any Dev op.
    if (od->dev != nd->dev)                           return -1;

    // A-3b (closes A-2d audit F2): rwx enforcement on dir mutation. POSIX
    // rename needs write + search (W|X) on BOTH parent dirs. Gated on the
    // Dev's perm_enforced (devramfs leaves .rename NULL; dev9p enforces from
    // A-3b). od->dev == nd->dev here, so one flag governs both.
    if (od->dev->perm_enforced) {
        struct t_stat ost, nst;
        if (spoor_stat_native(od, &ost) != 0)             return -1;
        if (perm_check(p, &ost, PERM_W | PERM_X) != 0)    return -1;
        if (spoor_stat_native(nd, &nst) != 0)             return -1;
        if (perm_check(p, &nst, PERM_W | PERM_X) != 0)    return -1;
    }

    return od->dev->rename(od, old_scratch, nd, new_scratch) == 0 ? 0 : -1;
}

static s64 sys_unlink_handler(u64 parent_fd_raw, u64 name_va, u64 name_len_raw,
                               u64 flags_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Only 0 or SYS_UNLINK_REMOVEDIR permitted; any other bit -> -1 (so a future
    // flag cannot be silently dropped, same discipline as SYS_WALK_CREATE perm).
    if (flags_raw & ~(u64)SYS_UNLINK_REMOVEDIR)       return -1;

    char scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    if (sys_copy_component(name_va, name_len_raw, scratch) != 0) return -1;

    struct Spoor *c = sys_resolve_dir_wr(p, parent_fd_raw);
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->unlink)                  return -1;

    // A-3b (closes A-2d audit F2): W|X on the parent dir to remove an entry
    // (POSIX). Gated on perm_enforced (dev9p enforces from A-3b; devramfs
    // leaves .unlink NULL).
    if (c->dev->perm_enforced) {
        struct t_stat cst;
        if (spoor_stat_native(c, &cst) != 0)              return -1;
        if (perm_check(p, &cst, PERM_W | PERM_X) != 0)    return -1;
    }

    return c->dev->unlink(c, scratch, (u32)flags_raw) == 0 ? 0 : -1;
}

// =============================================================================
// SYS_WSTAT — chmod/chown MECHANISM (A-2a; IDENTITY-DESIGN.md §9.5). Apply the
// (mode, uid, gid) subset selected by the `valid` mask to an open Spoor via
// dev->wstat_native (dev9p -> Tsetattr). Register-passed (no user buffer).
//
// This is the mechanism only: the handle RIGHT_WRITE gate is the sole gate;
// the per-file rwx PERMISSION policy (owner-only chmod, CAP_HOSTOWNER chown)
// is A-2d (the kernel rwx-enforcement layer). I-22 stands -- no rwx enforcement
// exists yet to bypass. The value checks here are structural (mask sanity,
// mode in 0777 with setuid rejected per §S5, uid/gid != INVALID), not policy.
// =============================================================================

static int spoor_wstat_native(struct Spoor *c, u32 valid, u32 mode,
                              u32 uid, u32 gid) {
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->wstat_native)            return -1;
    return c->dev->wstat_native(c, valid, mode, uid, gid);
}

static s64 sys_wstat_handler(u64 hraw, u64 valid_raw, u64 mode_raw,
                             u64 uid_raw, u64 gid_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    u32 valid = (u32)valid_raw;
    u32 mode  = (u32)mode_raw;
    u32 uid   = (u32)uid_raw;
    u32 gid   = (u32)gid_raw;

    // Mask sanity: at least one known bit, no reserved bit (so a future
    // T_WSTAT_* extension cannot be silently dropped -- same discipline as
    // SYS_UNLINK's flags / SYS_WALK_CREATE's perm).
    if (valid == 0)                                  return -1;
    if (valid & ~(u32)T_WSTAT_VALID)                 return -1;

    // Per-field structural bounds. A field whose valid bit is clear is forced
    // to 0 before the Dev call (the server ignores it -- its valid bit is
    // clear -- but a defined 0 avoids passing a caller-controlled stale value).
    if (valid & T_WSTAT_MODE) {
        if (mode & ~(u32)T_WSTAT_MODE_MASK)          return -1;  // setuid/sgid/sticky + stray bits
    } else {
        mode = 0;
    }
    if (valid & T_WSTAT_UID) {
        if (uid == PRINCIPAL_INVALID)                return -1;
    } else {
        uid = 0;
    }
    if (valid & T_WSTAT_GID) {
        if (gid == GID_INVALID)                      return -1;
    } else {
        gid = 0;
    }

    // Rights gate: KOBJ_SPOOR with RIGHT_WRITE (setattr mutates metadata).
    // Mirrors SYS_FSTAT's lookup but with RIGHT_WRITE; rejects KOBJ_SRV.
    struct Handle *slot = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_WRITE);
    if (!slot)                                       return -1;
    if (slot->kind != KOBJ_SPOOR)                    return -1;

    struct Spoor *c = (struct Spoor *)slot->obj;

    // A-2d: the ownership-change policy (IDENTITY-DESIGN.md 3.7.1 + perm.c).
    // Gated on perm_enforced (dev9p deferred to A-3; devramfs .wstat_native is
    // NULL so SYS_WSTAT on it returns -1 below regardless -> this is dormant at
    // v1.0, exercised by perm_wstat_check's unit tests and activated with dev9p
    // at A-3). Reads the file's CURRENT owner, then applies the policy.
    if (c->dev && c->dev->perm_enforced) {
        struct t_stat cur;
        if (spoor_stat_native(c, &cur) != 0)                  return -1;
        if (perm_wstat_check(p, cur.uid, valid, gid) != 0)    return -1;
    }
    return spoor_wstat_native(c, valid, mode, uid, gid) == 0 ? 0 : -1;
}

// =============================================================================
// SYS_CHROOT — stamp the caller's territory root_spoor (P5-stratumd-stub-
// bringup-e2). Per CORVUS-DESIGN.md §10.1 ("chroot at v1.0; full pivot at
// v1.x") + ARCH §11.2.
//
// Thin SVC wrapper over territory_chroot. The kernel-internal C-API does
// the source_is_valid check, the idempotent same-pointer short-circuit,
// the spoor_ref-before-swap + spoor_clunk-after-swap ordering, and the
// MountRefcountConsistency invariant maintenance (specs/territory.tla::
// Chroot).
//
// Audit-trigger: touches `kernel/territory.c` (CLAUDE.md §25.4 — Territory).
// Adds no new mount-table edge (no I-3 / I-1 implications); the only
// invariant in play is MountRefcountConsistency, extended in the spec
// for this chunk to include the root_spoor contribution.
// =============================================================================

static s64 sys_chroot_handler(u64 spoor_fd_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!p->territory)                               return -1;

    // RIGHT_READ on the source: a chroot target's only purpose is to
    // serve as a walk source for SYS_WALK_OPEN(FROM_ROOT, ...). Without
    // READ the pivot is structurally inert (you cannot walk from it).
    // Mirrors SYS_MOUNT's source-rights gate exactly.
    struct Spoor *source = sys_lookup_spoor(p, (hidx_t)spoor_fd_raw, RIGHT_READ);
    if (!source)                                     return -1;

    // territory_chroot handles: idempotent same-pointer (returns 0
    // without ref bump), prior-root displacement (spoor_clunk the old),
    // spoor_ref of the new source. NULL source is rejected by
    // territory_chroot's own check (returns -1).
    if (territory_chroot(p->territory, source) != 0) return -1;
    return 0;
}

// SYS_SET_TID_ADDRESS — record the clear-child-tid address on the calling
// thread + return its tid (P6-pouch-kernel-auxv; storage wired by P6-pouch-
// threads sub-chunk 9). musl's __pthread_setup calls this once per thread
// at startup; the kernel stores tidptr on the Thread and, on thread exit
// (SYS_THREAD_EXIT / SYS_EXITS), atomically clears *tidptr + torpor-wakes
// on it so a joiner observes the exit.
//
// `tidptr_raw == 0` is the "unset" sentinel — clears the field. Any other
// value passes a user-VA bound + alignment check at storage time; an
// invalid tidptr causes the syscall to return -1 (caller likely buggy)
// rather than silently accepting it and faulting at exit time.
//
// At v1.0 a Thread's tid equals its kernel struct Thread.tid (per
// thread_create's monotonic g_next_tid). For the MAIN thread of a Proc
// this is NOT the pid in general — the main thread's tid is whatever
// g_next_tid was at proc_alloc-time-spawn (tid 1 for joey, etc.). The
// older returned-pid pattern (single-threaded Procs) was an aliasing
// approximation; sub-chunk 9 swaps to the real per-Thread tid, which is
// the value pthread_self / pthread_join expects to compare against.
static s64 sys_set_tid_address_handler(u64 tidptr_raw) {
    struct Thread *t = current_thread();
    if (!t)         return -1;
    struct Proc *p = t->proc;
    if (!p)         return -1;

    if (tidptr_raw != 0) {
        // Must be 4-byte aligned + within user VA bound. The kernel
        // exit-time store uses uaccess_store_u32, which requires the
        // alignment.
        if ((tidptr_raw & 0x3u) != 0)                    return -1;
        if (tidptr_raw >= UACCESS_USER_VA_TOP)            return -1;
    }
    t->clear_child_tid = tidptr_raw;
    return (s64)t->tid;
}

// P6-pouch-threads (sub-chunk 9): SYS_THREAD_SPAWN handler.
//
// Validates the four user-VA args, creates a new Thread in the caller's
// Proc via thread_create_user, makes it RUNNABLE via sched_ready, returns
// its tid. -EINVAL on argument validation failure / kproc caller;
// -ENOMEM on alloc failure. Linux/musl-numeric errnos so pouch's
// syscall_ret.c decodes them as -errno.
//
// Caller's Proc must have pgtable_root != 0 (i.e. a userspace Proc with
// an installed address space). Calls FROM kproc are rejected upstream by
// the very fact that kproc threads never execute SVC instructions; the
// pgtable_root == 0 check is a defense-in-depth catch.
static s64 sys_thread_spawn_handler(u64 entry_va, u64 sp_va,
                                    u64 arg_va, u64 tls_va) {
    struct Thread *t = current_thread();
    if (!t)                                          return -22; // -EINVAL
    struct Proc *p = t->proc;
    if (!p)                                          return -22;
    if (p->magic != PROC_MAGIC)                      return -22;
    if (p == kproc())                                return -22;
    if (p->pgtable_root == 0)                        return -22;

    // entry_va: must be non-NULL + 4-byte aligned + within user VA.
    //
    // The 4-byte alignment is LOAD-BEARING (F2 audit close): aarch64 has
    // a CPU-mandatory PC alignment check (always-on, no SCTLR bit
    // disables it) — a misaligned PC at the eret target instantly raises
    // an EC_PC_ALIGN synchronous exception, which the EL1 dispatcher
    // routes to extinction_with_addr("EL0 PC alignment fault"). Without
    // this syscall-layer check, ANY userspace caller could trivially ELE
    // the kernel by passing a 1- or 2-byte-aligned entry_va — the SVC
    // returns success, the eret fires, the CPU alignment check trips, the
    // kernel extincts. Convert to -EINVAL at the gate so misalignment
    // becomes a clean userspace error instead of a kernel-killing payload.
    if (entry_va == 0)                               return -22;
    if (entry_va & 0x3u)                              return -22;
    if (entry_va >= UACCESS_USER_VA_TOP)              return -22;

    // sp_va: AAPCS64 requires 16-byte stack alignment at function entry.
    // The pouch pthread layer is responsible for picking an aligned top
    // (the stack base + size; typical pthread stacks are page-aligned so
    // the top is too). Non-zero check + alignment + bound.
    //
    // F8 audit close: tighten the bound to `>=` (was `>`). Accepting
    // sp_va == UACCESS_USER_VA_TOP was marginally legal (the first push
    // writes BELOW the SP, so downward writes stay in user VA) but
    // created a fragile boundary: any compiler-emitted prologue using
    // `[sp, #+N]` (rare but ABI-permitted for register-save slots) would
    // dereference at a TTBR1 address. Matches entry_va's strict `>=`.
    if (sp_va == 0)                                   return -22;
    if ((sp_va & 0xFu) != 0)                          return -22;
    if (sp_va >= UACCESS_USER_VA_TOP)                 return -22;

    // tls_va: 0 is permitted (no TLS yet — the entry can set it
    // afterward via msr tpidr_el0). Non-zero must be in user VA. No
    // alignment requirement — TLS layout is libc-defined.
    if (tls_va != 0 && tls_va >= UACCESS_USER_VA_TOP) return -22;

    struct Thread *nt = thread_create_user(p, entry_va, sp_va, arg_va, tls_va);
    if (!nt)                                         return -12; // -ENOMEM

    // ready() inserts the new RUNNABLE Thread into the run-tree. From
    // here it can be picked by any CPU on the next sched() tick.
    ready(nt);
    return (s64)nt->tid;
}

// P6-pouch-threads (sub-chunk 9): SYS_THREAD_EXIT handler. Wraps
// thread_exit_self (in kernel/proc.c) which never returns.
__attribute__((noreturn))
static void sys_thread_exit_handler(void) {
    thread_exit_self();
    // thread_exit_self is __noreturn; the wrapper inherits via the
    // attribute, so the compiler does not emit a fall-through.
    __builtin_unreachable();
}

// =============================================================================
// P6-pouch-signals-impl (sub-chunk 13a): note delivery syscalls.
// =============================================================================
//
// Five thin handlers over kernel/notes.c + kernel/devnotes.c. Design in
// ARCH §7.6.1-§7.6.8.

extern int  notes_noted_restore(struct exception_context *ctx,
                                struct Thread *t);
__attribute__((noreturn))
extern void notes_noted_default(struct Thread *t);

// SYS_NOTE_OPEN — mint a fd to the calling Proc's note queue.
//   (no args)
//
// Mints a fresh Spoor (via devnotes->attach), opens it, installs in the
// caller's handle table. Idempotent across calls — each open mints a
// separate Spoor, but reads/polls all access the same per-Proc queue
// (devnotes is stateless; the queue is in current_thread()->proc->notes).
static s64 sys_note_open_handler(void) {
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;
    struct Proc *p = t->proc;

    struct Spoor *c = devnotes.attach(NULL);
    if (!c) return -1;
    struct Spoor *opened = devnotes.open(c, 0 /* OREAD */);
    if (!opened) {
        spoor_unref(c);
        return -1;
    }

    rights_t rights = RIGHT_READ;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, rights, opened);
    if (fd < 0) {
        // handle_alloc failed; release the Spoor we just opened. The
        // close runs the dev->close path (devnotes_close → dev_simple_close).
        spoor_clunk(opened);
        return -1;
    }
    return (s64)fd;
}

// SYS_NOTIFY(handler_va) — register/clear the async note handler.
// F9 audit close: release-store so a multi-thread Proc's other Thread
// observing handler_va in notes_deliver_at_el0_return sees a coherent
// value (paired with the acquire-load in notes.c).
static s64 sys_notify_handler(u64 handler_va) {
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;
    struct Proc *p = t->proc;

    if (handler_va != 0) {
        if (handler_va >= UACCESS_USER_VA_TOP) return -1;
        if (handler_va & 0x3) return -1;     // aarch64 instructions are 4-aligned
    }
    __atomic_store_n(&p->handler_va, handler_va, __ATOMIC_RELEASE);
    return 0;
}

// SYS_NOTED(ctx, arg) — return from a handler.
//   arg = 0 (NCONT) — restore ctx from t->note_saved_*
//   arg = 1 (NDFLT) — exits with the note name
// On invalid arg / not-in-handler, sets ctx->regs[0] = -1 + returns.
static void sys_noted_handler(struct exception_context *ctx, u64 arg) {
    struct Thread *t = current_thread();
    if (!t || !t->proc) {
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
    if (!t->in_handler) {
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
    if (arg == 0) {
        // NCONT — restore. Returns 0 on success (ctx rewritten with the
        // pre-handler user state; regs[0] is now the saved value).
        if (notes_noted_restore(ctx, t) != 0) {
            ctx->regs[0] = (u64)(s64)-1;
        }
        return;
    }
    if (arg == 1) {
        // NDFLT -- default action. For the v1.0 supported set every
        // default is exits(name), which requires live_peers == 0
        // (exits extincts on live peer threads -- cross-thread shoot-
        // down is v1.x).
        //
        // R3-F2 audit close: refuse NDFLT in multi-thread Procs so the
        // kernel does not extinct on userspace-driven path. The user's
        // handler can fall back to NCONT (restore) or do its own
        // cleanup. Single-thread Procs reach exits cleanly.
        irq_state_t s = proc_table_lock_acquire();
        int live_peers = proc_count_live_peers_locked(t->proc, t);
        proc_table_lock_release(s);
        if (live_peers != 0) {
            ctx->regs[0] = (u64)(s64)-1;
            return;
        }
        notes_noted_default(t);
        // unreachable
    }
    // Anything else — -EINVAL via -1.
    ctx->regs[0] = (u64)(s64)-1;
}

// SYS_POSTNOTE(pid, name_va, name_len) — post a note to another Proc.
// Permission gate at v1.0: caller must be the target's parent OR
// pid == self_pid (self-post is always allowed).
#define NOTES_POSTNOTE_NAME_MAX  (NOTE_NAME_MAX - 1)

struct postnote_walk_ctx {
    int           target_pid;
    struct Proc  *caller;
    const char   *name;
    int           result;        // 0 = not yet found (continue walk);
                                  // +1 = post succeeded (stop walk);
                                  // -1 = post failed / permission denied
                                  //      (stop walk).
};

static int postnote_walk_cb(struct Proc *target, void *arg) {
    struct postnote_walk_ctx *w = (struct postnote_walk_ctx *)arg;
    if (target->pid != w->target_pid) return 0;     // keep walking

    // Found the target. Permission gate: caller must be the parent.
    if (target->parent != w->caller) {
        w->result = -1;
        return 1;     // stop walk (non-zero return)
    }

    // R2-F9 audit close: refuse posts to non-ALIVE targets. A target in
    // ZOMBIE/INVALID state has no consumer (no thread will EL0-return
    // ever again); the post would be wasted work that returns success
    // misleadingly. wait_pid is the proper channel for ZOMBIE state.
    if (target->state != PROC_STATE_ALIVE) {
        w->result = -1;
        return 1;
    }

    // SYS_EXIT_GROUP / kill cross-thread shootdown (ARCH §7.9.1, I-24): a
    // multi-thread target is no longer refused (the prior `kill -> -EIO in
    // multi-thread Proc`, 13b R1-F9). Cascade-terminate the whole Proc --
    // proc_group_terminate flags it + wakes/kicks its Threads so each
    // self-exits at its EL0-return die-check; the last Thread out reaps the
    // Proc. Safe under g_proc_table_lock (held by proc_for_each here):
    // proc_group_terminate acquires only torpor / rendez / cs locks, all below
    // proc_table_lock in the order, and the target is alive under this lock so
    // there is no reap-UAF. A SINGLE-thread target falls through to the note
    // post below -- the existing non-catchable-kill EL0-return delivery, left
    // unchanged.
    if (notes_name_is_kill(w->name)) {
        int live_threads = proc_count_live_peers_locked(target, NULL);
        if (live_threads > 1) {
            proc_group_terminate(target, "killed");
            w->result = 1;
            return 1;
        }
    }

    // Post. notes_post is safe under proc_table_lock -- see the lock-order
    // discussion in sys_postnote_handler.
    int rc = notes_post(target, w->name, 0u, w->caller, false);
    w->result = (rc == 0) ? 1 : -1;
    return 1;
}

static s64 sys_postnote_handler(u64 pid_raw, u64 name_va, u64 name_len_raw) {
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;
    struct Proc *p = t->proc;

    if (name_len_raw == 0) return -1;
    if (name_len_raw > NOTES_POSTNOTE_NAME_MAX) return -1;
    if (name_va == 0) return -1;
    if (name_va >= UACCESS_USER_VA_TOP) return -1;
    if (name_va + name_len_raw > UACCESS_USER_VA_TOP) return -1;

    char buf[NOTE_NAME_MAX];
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) buf[i] = 0;
    for (u32 i = 0; i < (u32)name_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(name_va + i, &b) != 0) return -1;
        // Reject embedded NUL and non-printable bytes — the v1.0
        // supported set is "interrupt"/"kill"/"pipe"/"child_exit", all
        // [a-z_]; the validation in notes_post will catch unknown
        // strings, but reject obvious garbage at the boundary.
        if (b == 0) return -1;
        if (b < 0x20 || b > 0x7e) return -1;
        buf[i] = (char)b;
    }
    buf[(u32)name_len_raw] = 0;

    int target_pid = (int)pid_raw;

    // Fast-path self-post: we ARE the target. No lookup needed; the Proc
    // can't be freed while we're running it. pid_raw == 0 is the
    // self-post sentinel (P6-pouch-signals sub-chunk 13b): pouch's
    // raise() has no userspace getpid path at v1.0, so it passes 0 and
    // relies on this kernel-side mapping. POSIX semantics: kill(0, sig)
    // means "send to every process in the calling process's group" —
    // Thylacine has no process groups at v1.0, so the closest equivalent
    // is "send to my own Proc" and the sentinel-shaped collapse is
    // POSIX-conforming. The sentinel is documented as ABI in
    // kernel/include/thylacine/syscall.h (SYS_POSTNOTE docblock).
    //
    // SYS_EXIT_GROUP / kill cross-thread shootdown (ARCH §7.9.1, I-24): a
    // multi-thread self-kill cascades the whole Proc instead of being refused
    // (the prior `kill -> -EIO`, 13b R1-F9). proc_group_terminate's universal
    // death-wake walks p->threads and so MUST run UNDER g_proc_table_lock
    // (#811, ARCH §8.8.1) -- the count AND the cascade run in the SAME lock
    // acquisition (previously the lock was dropped before the cascade; that is
    // now a contract violation). self cannot be reaped while running here; the
    // caller returns success + self-exits at its own EL0-return die-check
    // before userspace resumes. A single-thread self-post falls through to the
    // note post below (unchanged).
    if (target_pid == p->pid || pid_raw == 0) {
        if (notes_name_is_kill(buf)) {
            irq_state_t s = proc_table_lock_acquire();
            int live_threads = proc_count_live_peers_locked(p, NULL);
            if (live_threads > 1) {
                proc_group_terminate(p, "killed");
                proc_table_lock_release(s);
                return 0;
            }
            proc_table_lock_release(s);
        }
        int rc = notes_post(p, buf, 0u, p, false);
        return (rc == 0) ? 0 : (s64)-1;
    }

    // Cross-Proc post: walk the proc tree via proc_for_each, which runs
    // its callback under g_proc_table_lock. We do the find + permission-
    // check + post inside the callback so the target Proc cannot be
    // reaped + freed mid-operation. Lock order: proc_table_lock → q->lock
    // → poll_list.lock → (drop q->lock, still hold proc_table_lock) →
    // rendez.lock. None of those reverse-takes proc_table_lock so the
    // chain is acyclic.
    struct postnote_walk_ctx wctx = {
        .target_pid = target_pid,
        .caller     = p,
        .name       = buf,
        .result     = 0,
    };
    int rv = proc_for_each(postnote_walk_cb, &wctx);
    (void)rv;     // proc_for_each returns the first non-zero callback
                  // result; wctx.result carries the canonical answer
    if (wctx.result == 1) return 0;     // post succeeded
    return -1;                            // not found / permission / EAGAIN
}

// SYS_NOTE_MASK(new_mask, old_mask_out_va) — swap-and-return mask.
static s64 sys_note_mask_handler(u64 new_mask, u64 old_mask_out_va) {
    struct Thread *t = current_thread();
    if (!t) return -1;

    u64 old = t->note_mask;
    t->note_mask = new_mask;

    if (old_mask_out_va != 0) {
        // F7 audit close: bound the END of the 8-byte writeback. The
        // prior code checked only the START, letting the high bytes
        // straddle the user/kernel boundary and (with PAN unconfigured
        // at v1.0) write attacker-controlled bytes (the old mask) into
        // kernel memory. Both bounds + overflow-check now.
        if (old_mask_out_va >= UACCESS_USER_VA_TOP ||
            old_mask_out_va + sizeof(u64) > UACCESS_USER_VA_TOP ||
            old_mask_out_va + sizeof(u64) < old_mask_out_va) {
            // Restore the prior mask on bound failure so the syscall is
            // observably atomic (no half-swap).
            t->note_mask = old;
            return -1;
        }
        // Byte-by-byte uaccess (no u64 primitive at v1.0 — uaccess.h
        // exposes u8 / u32 only).
        const u8 *src = (const u8 *)&old;
        for (u32 i = 0; i < sizeof(u64); i++) {
            if (uaccess_store_u8(old_mask_out_va + i, src[i]) != 0) {
                t->note_mask = old;
                return -1;
            }
        }
    }

    // A mask CLEAR re-pumps delivery — a note that was previously
    // deferred for this Thread may now be deliverable. The actual
    // delivery fires at the next EL0-return tail (which is the syscall
    // return we're about to ret from), so the EL0-return-tail check
    // will see the new mask state. No explicit wake needed here.
    return 0;
}

// =============================================================================
// SYS_BURROW_ATTACH / SYS_BURROW_DETACH — the anonymous-memory syscalls
// (P6-pouch-mem). The v1.0 native memory-growth primitive — ARCHITECTURE.md
// §6.5 Tier 1. SYS_BURROW_ATTACH picks a free VA in the burrow-attach window
// and installs an anonymous RW Burrow; SYS_BURROW_DETACH tears one down.
// =============================================================================
//
// Both run their VMA-list work under p->vma_lock. At v1.0 Procs are single-
// threaded so the lock is uncontended by construction; it is held so the
// find-gap + vma_insert sequence (attach) and the lookup + vma_remove
// sequence (detach) are atomic once the pouch-threads sub-chunk makes Procs
// multi-threaded. Plain spin_lock (not irqsave): no IRQ handler touches a
// Proc's VMA list, and the critical section never sleeps — burrow_create_anon
// / vma_alloc are non-blocking allocations (NULL on OOM, never wait).
//
// F2 (P6-pouch-mem-a audit, P3, deferred): attach holds vma_lock across
// burrow_create_anon's eager (up to BURROW_ATTACH_MAX) page allocation.
// At v1.0 the lock is uncontended so this is free; the pouch-threads
// sub-chunk — which introduces real contention — narrows the hold (the
// find-gap result is advisory, re-validated by vma_insert's overlap
// reject, so burrow_create_anon can move out of the critical section).
//
// The _for_proc inners carry the logic with an explicit Proc so the
// kernel test harness can drive them on a fresh proc_alloc'd Proc; the
// SVC handlers are the thin current_thread() wrappers (the
// sys_pipe_for_proc pattern).

s64 sys_burrow_attach_for_proc(struct Proc *p, u64 length_raw) {
    if (!p)                                          return -1;

    // Bound the request before rounding so length + PAGE_SIZE - 1 below
    // cannot overflow. BURROW_ATTACH_MAX is page-aligned, so the rounded
    // length never exceeds it either.
    if (length_raw == 0)                             return -1;
    if (length_raw > BURROW_ATTACH_MAX)              return -1;

    // Round up to a whole number of pages — the VMA and the Burrow both
    // work in page units.
    u64 length = (length_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);

    spin_lock(&p->vma_lock);

    // Pick a free VA — first-fit in the burrow-attach window. The gap is
    // chosen and the VMA installed under one lock hold, so a sibling
    // thread's concurrent attach cannot claim the same gap.
    u64 vaddr;
    if (vma_find_gap(p, length, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &vaddr) != 0) {
        spin_unlock(&p->vma_lock);
        return -1;
    }

    // burrow_create_anon: handle_count = 1 (the construction reference),
    // mapping_count = 0; pages allocated eagerly (power-of-2 rounded).
    struct Burrow *b = burrow_create_anon(length);
    if (!b) {
        spin_unlock(&p->vma_lock);
        return -1;
    }

    // burrow_map installs the VMA (vma_alloc → burrow_acquire_mapping,
    // mapping_count → 1). Then drop the construction handle: handle_count
    // → 0, mapping_count = 1 keeps the Burrow alive — the exec.c
    // discipline (Tier 1: no handle, the VMA owns the Burrow). On
    // burrow_map failure the construction handle is the only reference;
    // burrow_unref frees the Burrow (mapping_count still 0).
    if (burrow_map(p, b, vaddr, length, VMA_PROT_RW) != 0) {
        burrow_unref(b);
        spin_unlock(&p->vma_lock);
        return -1;
    }
    burrow_unref(b);

    spin_unlock(&p->vma_lock);

    // vaddr is in [EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP) — far
    // below the s64 sign bit, so a valid base is never mistaken for -1.
    return (s64)vaddr;
}

static s64 sys_burrow_attach_handler(u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    return sys_burrow_attach_for_proc(t->proc, length_raw);
}

s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw) {
    if (!p)                                          return -1;
    if (length_raw == 0)                             return -1;
    if (length_raw > BURROW_ATTACH_MAX)              return -1;
    if (vaddr_raw & (PAGE_SIZE - 1))                 return -1;

    // Same page-rounding as SYS_BURROW_ATTACH, so a caller may pass
    // either its original request or the rounded length and still match
    // the installed VMA's span.
    u64 length = (length_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);

    // Confine detach to the burrow-attach window (F1, P6-pouch-mem-a
    // audit). burrow_unmap matches a VMA by geometry alone — without
    // this bound a caller could pass the coordinates of its own ELF
    // segment, stack, or stack-guard VMA and have burrow_unmap dismantle
    // it (removing the stack guard silently retires a security page).
    // Every burrow_attach region lives in the window and every ELF /
    // stack / guard VMA sits below it, so the bound structurally
    // excludes them. Overflow-safe: length <= BURROW_ATTACH_MAX, far
    // below EXEC_USER_BURROW_TOP, so TOP - length never underflows.
    if (vaddr_raw < EXEC_USER_BURROW_BASE)           return -1;
    if (vaddr_raw > EXEC_USER_BURROW_TOP - length)   return -1;

    spin_lock(&p->vma_lock);
    // burrow_unmap exact-matches [vaddr, vaddr + length) against an
    // installed VMA (no partial detach at v1.0), removes it, and frees
    // the Burrow's pages — mapping_count reaches 0 with handle_count
    // already 0.
    int rc = burrow_unmap(p, vaddr_raw, length);
    spin_unlock(&p->vma_lock);

    return (s64)rc;
}

static s64 sys_burrow_detach_handler(u64 vaddr_raw, u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    return sys_burrow_detach_for_proc(t->proc, vaddr_raw, length_raw);
}

// P6-pouch-wait-addr (sub-chunk 8): SYS_TORPOR_WAIT / SYS_TORPOR_WAKE
// SVC handlers — thin `current_thread()` wrappers over the testable
// `_for_proc` inners in `kernel/torpor.c`. timeout_us is signed s64
// (negative = block indefinitely); the syscall arg is delivered as u64
// so we round-trip through (s64) at the wrapper.
static s64 sys_torpor_wait_handler(u64 addr_va, u64 expected_raw,
                                   u64 timeout_us_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return TORPOR_ERR_EINVAL;
    return sys_torpor_wait_for_proc(t->proc, addr_va,
                                    (u32)expected_raw,
                                    (s64)timeout_us_raw);
}

static s64 sys_torpor_wake_handler(u64 addr_va, u64 count_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return TORPOR_ERR_EINVAL;
    return sys_torpor_wake_for_proc(t->proc, addr_va, (u32)count_raw);
}

static s64 sys_dup_handler(u64 hraw, u64 new_rights_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    // handle_dup validates rights subset internally; the static_assert
    // on RIGHT_ALL bounds the legal bit-set range.
    if (new_rights_raw & ~(u64)RIGHT_ALL)             return -1;
    hidx_t nh = handle_dup(p, (hidx_t)hraw, (rights_t)new_rights_raw);
    return (s64)nh;
}

// =============================================================================
// SYS_MOUNT / SYS_UNMOUNT — graft / remove a Spoor in the caller's Territory
// mount table (P5-mount-syscall).
// =============================================================================
//
// User-visible body of `mount(source_spoor_fd, target_path, flags) → 0`
// and `unmount(target_path)` per ARCH §9.6.1 + §11.2. Thin SVC wrappers
// over `kernel/territory.c::mount` and `::unmount`.
//
// The mount-table primitive does the per-entry spoor_ref / spoor_unref
// + the MountRefcountConsistency invariant (specs/territory.tla). The
// SVC handlers' job is the user-facing checks: look up the KOBJ_SPOOR
// fd, validate flags + rights, and route into the C-API.
//
// Composition with SYS_ATTACH_9P: a dev9p-backed root Spoor from
// SYS_ATTACH_9P has its dev9p_priv.attached_owner populated; closing
// its last fd tears down the 9P session. SYS_MOUNT bumps the Spoor's
// refcount, so even if the caller closes their attach_9p fd after
// mounting, the mount-table entry's ref keeps the session alive. The
// session is torn down only when unmount() (or Territory destruction)
// drops the LAST reference.

// MREPL / MBEFORE / MAFTER / MCREATE are 0x0001 / 0x0002 / 0x0004 /
// 0x0008 per territory.h. Mask out everything else — userspace
// supplying junk bits is rejected at the syscall layer (mount() in
// territory.c is silent on extra bits, but we want a tight contract
// at the boundary).
#define SYS_MOUNT_VALID_FLAGS  ((u32)(MREPL | MBEFORE | MAFTER | MCREATE))

// Inner — testable kernel-internally with a Proc handle. Returns 0 on
// success, -1 on any failure. Mirrors the sys_pipe_for_proc /
// sys_write_for_proc shape.
//
// `target` is a path_id_t (u32 abstract token at v1.0). Future
// string-path resolution lands with the fd-syscall walk subsystem;
// when it does, this inner gains a kernel-resolved path_id_t arg and
// the SVC wrapper does the walk + copy.
int sys_mount_for_proc(struct Proc *p, hidx_t source_fd,
                       path_id_t target, u32 flags) {
    if (!p)                                          return -1;
    if (!p->territory)                               return -1;
    if (flags & ~SYS_MOUNT_VALID_FLAGS)               return -1;

    // RIGHT_READ on the source: a mount holder consumes the source's
    // tree (walks it, reads files through it). A handle without READ
    // is structurally useless as a mount source. RIGHT_TRANSFER is
    // separately required for cross-Proc transfer surfaces (Phase 5+),
    // not for the mount installation itself.
    struct Spoor *source = sys_lookup_spoor(p, source_fd, RIGHT_READ);
    if (!source)                                     return -1;

    // territory.c::mount handles: idempotency (no-op on duplicate),
    // MREPL (replace existing entry), full-table rejection, and the
    // per-entry spoor_ref. Returns 0 / -1 (NULL source — already
    // ruled out above) / -2 (table full). We collapse to 0 / -1.
    if (mount(p->territory, source, target, flags) != 0) return -1;
    return 0;
}

// Inner — testable kernel-internally. Returns 0 on success, -1 if no
// entry exists at target_path.
int sys_unmount_for_proc(struct Proc *p, path_id_t target) {
    if (!p)                                          return -1;
    if (!p->territory)                               return -1;
    if (unmount(p->territory, target) != 0)           return -1;
    return 0;
}

static s64 sys_mount_handler(u64 source_fd_raw, u64 target_raw, u64 flags_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    // path_id_t is u32; reject values that don't fit so a buggy
    // caller's misuse of x1 (e.g., a stray VA) doesn't quietly
    // alias to a small u32. mount-table entries keyed on path_id_t
    // must round-trip through u32 unchanged.
    if (target_raw > (u64)0xFFFFFFFFu)                return -1;
    if (flags_raw  > (u64)0xFFFFFFFFu)                return -1;
    return (s64)sys_mount_for_proc(p, (hidx_t)source_fd_raw,
                                   (path_id_t)target_raw,
                                   (u32)flags_raw);
}

static s64 sys_unmount_handler(u64 target_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (target_raw > (u64)0xFFFFFFFFu)                return -1;
    return (s64)sys_unmount_for_proc(p, (path_id_t)target_raw);
}

// =============================================================================
// P5-corvus-syscalls: v1.0 hardening syscalls (CORVUS-DESIGN.md §4.1.1).
// =============================================================================
//
// Each syscall sets a one-way per-Proc flag (PROC_FLAG_*) or performs
// a one-shot action. Consumed by corvus + per-user stratumd at startup
// to satisfy CORVUS-DESIGN invariants C-2 (mlock + dumpable) and the
// CSPRNG-seeded discipline C-15.

// SYS_MLOCKALL — pin pages. CAP_LOCK_PAGES required. Sets PROC_FLAG_MLOCKED.
// v1.0 has no swap; the flag is forward-compat scaffolding.
int sys_mlockall_for_proc(struct Proc *p, u32 flags) {
    if (!p)                                          return -1;
    (void)flags;       // unused at v1.0; reserved for MCL_CURRENT/MCL_FUTURE
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_LOCK_PAGES) == 0)
        return -1;
    // Atomic OR: A-4c-2 made PROC_FLAG_CONSOLE_ATTACHED (same word) multi-writer
    // (the SAK kthread clears it on the console owner). If `p` is the console
    // owner, a non-atomic RMW here could race the SAK's atomic clear and lose it
    // (I-27). So every proc_flags RMW is atomic.
    __atomic_or_fetch(&p->proc_flags, PROC_FLAG_MLOCKED, __ATOMIC_RELAXED);
    return 0;
}

static s64 sys_mlockall_handler(u64 flags_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (flags_raw > (u64)0xFFFFFFFFu)                 return -1;
    return (s64)sys_mlockall_for_proc(p, (u32)flags_raw);
}

// SYS_SET_DUMPABLE — control core-dump permission. One-way to 0.
// Setting to 1 from a Proc that already has PROC_FLAG_NODUMP is REFUSED.
int sys_set_dumpable_for_proc(struct Proc *p, u32 dumpable) {
    if (!p)                                          return -1;
    if (dumpable == 0) {
        // Atomic RMW: the console bit shares this word and is multi-writer
        // post-A-4c-2 (see sys_mlockall_for_proc).
        __atomic_or_fetch(&p->proc_flags, PROC_FLAG_NODUMP, __ATOMIC_RELAXED);
        return 0;
    }
    if (dumpable == 1) {
        // Refuse re-enable: corvus's no-coredump posture is one-way.
        if (__atomic_load_n(&p->proc_flags, __ATOMIC_RELAXED) & PROC_FLAG_NODUMP)
            return -1;
        return 0;                                     // already dumpable
    }
    return -1;                                       // bad arg
}

static s64 sys_set_dumpable_handler(u64 dumpable_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (dumpable_raw > (u64)0xFFFFFFFFu)              return -1;
    return (s64)sys_set_dumpable_for_proc(p, (u32)dumpable_raw);
}

// SYS_SET_TRACEABLE — control debug-Spoor attach permission. One-way to 0.
int sys_set_traceable_for_proc(struct Proc *p, u32 traceable) {
    if (!p)                                          return -1;
    if (traceable == 0) {
        // Atomic RMW: the console bit shares this word and is multi-writer
        // post-A-4c-2 (see sys_mlockall_for_proc).
        __atomic_or_fetch(&p->proc_flags, PROC_FLAG_NOTRACE, __ATOMIC_RELAXED);
        return 0;
    }
    if (traceable == 1) {
        if (__atomic_load_n(&p->proc_flags, __ATOMIC_RELAXED) & PROC_FLAG_NOTRACE)
            return -1;
        return 0;
    }
    return -1;
}

static s64 sys_set_traceable_handler(u64 traceable_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (traceable_raw > (u64)0xFFFFFFFFu)              return -1;
    return (s64)sys_set_traceable_for_proc(p, (u32)traceable_raw);
}

// SYS_EXPLICIT_BZERO — compiler-barrier'd memset of a user-VA buffer.
// Bounce through a kernel-stack scratch + uaccess_store_u8 per byte.
// The "compiler barrier" property is delivered by the per-byte
// uaccess_store_u8 path — the compiler cannot prove the writes are
// dead because they cross the kernel/user boundary.
static s64 sys_explicit_bzero_handler(u64 buf_va, u64 len) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -1;
    if (len > SYS_RW_MAX) len = SYS_RW_MAX;
    if (len == 0)                                    return 0;

    for (u64 i = 0; i < len; i++) {
        if (uaccess_store_u8(buf_va + i, 0) != 0)    return -1;
    }
    return 0;
}

// SYS_GETRANDOM — read kernel CSPRNG bytes into a user-VA buffer.
// CAP_CSPRNG_READ required. Caller's user-VA buffer is filled via
// 4 KiB kernel-stack scratch + uaccess_store_u8 per byte (mirrors
// SYS_READ's bounce pattern).
//
// CSPRNG-seeded check (C-15): if kern_random_seeded() is false, returns
// -1 immediately. The GRND_NONBLOCK flag is effectively v1.0's only
// mode — v1.x adds a real blocking primitive if/when software-CSPRNG
// mixing introduces an unseeded state.
static s64 sys_getrandom_handler(u64 buf_va, u64 len, u64 flags_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_CSPRNG_READ) == 0)
        return -1;
    if (flags_raw > (u64)0xFFFFFFFFu)                 return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -1;
    if (len > SYS_RW_MAX) len = SYS_RW_MAX;
    if (len == 0)                                    return 0;
    if (!kern_random_seeded())                       return -1;

    u8 scratch[SYS_RW_MAX];
    long got = kern_random_bytes(scratch, (long)len);
    if (got != (long)len)                            return -1;

    for (u64 i = 0; i < len; i++) {
        if (uaccess_store_u8(buf_va + i, scratch[i]) != 0) {
            // F237: partial-fault scrubbing. Entropy was already written
            // for bytes [0..i); a caller observing -1 must not be able
            // to read those bytes as valid CSPRNG output. Zero the
            // partial range (best-effort — same uaccess path could fail
            // again; the kernel's discipline is best-effort, not atomic).
            // Also zero the scratch to drop kernel-side state.
            for (u64 j = 0; j < i; j++) {
                (void)uaccess_store_u8(buf_va + j, 0);
            }
            for (u64 j = 0; j < len; j++) scratch[j] = 0;
            return -1;
        }
    }
    return (s64)len;
}

// =============================================================================
// SYS_SPAWN — combined rfork(RFPROC) + exec on a devramfs binary (P5-spawn-wait).
// =============================================================================
//
// ABI: x0 = name_va, x1 = name_len → child_pid (>0) / -1.
//
// The kernel-internal rfork(RFPROC, entry, arg) takes a C entry function;
// the child runs entry(arg) on a kthread kstack and (for userspace
// children) is expected to call exec_setup + userland_enter before
// returning. SYS_SPAWN is the smallest user-facing primitive that fits
// that mold: name-by-devramfs + child runs the named binary. v1.0 has
// no SYS_RFORK (which would require COW + child-context restoration);
// adding it later is a separate chunk.
//
// Lifetime of the ELF blob copy: kmalloc'd by the SYS_SPAWN handler,
// freed by the child's spawn_thunk after exec_setup. The args struct is
// also kmalloc'd + freed (lives across the rfork boundary, so it can't
// be on the caller's kernel stack — the caller may return to userspace
// before the child's thunk runs).

struct spawn_args {
    void   *blob;       // kmalloc'd 8-aligned copy of the ELF; thunk frees
    size_t  blob_size;
};

__attribute__((noreturn))
static void sys_spawn_thunk(void *arg) {
    struct spawn_args *sa = (struct spawn_args *)arg;
    void *blob       = sa->blob;
    size_t blob_size = sa->blob_size;
    kfree(sa);

    struct Thread *t = current_thread();
    if (!t) extinction("sys_spawn_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("sys_spawn_thunk: no proc");

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, blob, blob_size, &entry, &sp);
    kfree(blob);
    if (rc != 0) {
        // Surfaces as exit_status=1 in the parent's SYS_WAIT_PID.
        exits("fail-exec");
    }

    userland_enter(entry, sp);
}

// R15 F231 close: helper used by both spawn-with-fds and spawn-full to
// look up each fd, verify KOBJ_SPOOR, bump spoor_ref, and CAPTURE the
// parent slot's rights for the child-side install. Returns 0 on
// success with bumped[] + bumped_rights[] populated (bumped_count = fd_count);
// returns -1 on any failure with all in-flight bumps dropped.
//
// Note: bumped[] and bumped_rights[] are caller-allocated arrays of
// SYS_SPAWN_MAX_FDS entries each.
static int sys_bump_inherit_fds(struct Proc *p, const u32 *fds, u32 fd_count,
                                struct Spoor *bumped[SYS_SPAWN_MAX_FDS],
                                rights_t bumped_rights[SYS_SPAWN_MAX_FDS]) {
    u32 bumped_count = 0;
    for (u32 i = 0; i < fd_count; i++) {
        struct Handle *slot = handle_get(p, (hidx_t)fds[i]);
        if (!slot || slot->kind != KOBJ_SPOOR) {
            for (u32 j = 0; j < bumped_count; j++) spoor_unref(bumped[j]);
            return -1;
        }
        struct Spoor *s = (struct Spoor *)slot->obj;
        spoor_ref(s);
        bumped[bumped_count]        = s;
        bumped_rights[bumped_count] = slot->rights;
        bumped_count++;
    }
    return 0;
}

// Kernel-side body: takes a kernel-resident NUL-terminated name and the
// caller's Proc (used for context; the rfork creates a fresh child Proc
// regardless of the caller). Exported (non-static) for kernel-internal
// tests in kernel/test/test_sys_spawn.c.
int sys_spawn_for_proc(struct Proc *p, const char *name, size_t name_len) {
    if (!p)                                            return -1;
    if (!name)                                         return -1;
    if (name_len == 0 || name_len > SYS_SPAWN_NAME_MAX) return -1;
    // Reject embedded NUL in the in-band range: name_len bytes must all
    // be non-NUL; name[name_len] must be NUL (caller's contract).
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\0')                            return -1;
    }
    if (name[name_len] != '\0')                         return -1;

    const void *cpio_blob = NULL;
    size_t      cpio_size = 0;
    if (devramfs_lookup(name, &cpio_blob, &cpio_size) != 0) return -1;
    if (cpio_size == 0 || cpio_size > SYS_SPAWN_BLOB_MAX)   return -1;

    // 8-aligned copy of the cpio bytes. The cpio data is 4-aligned but
    // elf_load demands 8 (R5-G F61). kmalloc returns naturally aligned
    // memory for power-of-two requests; >2 KiB routes through alloc_pages,
    // which is page-aligned (4096 → 8-aligned).
    void *blob_copy = kmalloc(cpio_size, 0);
    if (!blob_copy)                                    return -1;
    if (((uintptr_t)blob_copy & 0x7) != 0) {
        // Shouldn't happen given kmalloc's contract, but assert defensively.
        kfree(blob_copy);
        return -1;
    }
    const u8 *src = (const u8 *)cpio_blob;
    u8 *dst = (u8 *)blob_copy;
    for (size_t i = 0; i < cpio_size; i++) dst[i] = src[i];

    struct spawn_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        kfree(blob_copy);
        return -1;
    }
    sa->blob      = blob_copy;
    sa->blob_size = cpio_size;

    int pid = rfork(RFPROC, sys_spawn_thunk, sa);
    if (pid < 0) {
        kfree(sa);
        kfree(blob_copy);
        return -1;
    }
    return pid;
}

static s64 sys_spawn_handler(u64 name_va, u64 name_len_raw) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    if (name_len_raw == 0 || name_len_raw > SYS_SPAWN_NAME_MAX) return -1;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;

    char name[SYS_SPAWN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b = 0;
        if (uaccess_load_u8(name_va + i, &b) != 0)     return -1;
        if (b == 0)                                    return -1;  // embedded NUL
        name[i] = (char)b;
    }
    name[name_len_raw] = '\0';

    return (s64)sys_spawn_for_proc(p, name, (size_t)name_len_raw);
}

// =============================================================================
// SYS_SPAWN_WITH_FDS — spawn with inherited Spoor fds (P5-stratumd-stub-b).
// =============================================================================
//
// Extends SYS_SPAWN with explicit fd inheritance: the named fds (each
// must be KOBJ_SPOOR at v1.0) are installed in the child's handle table
// at slots 0..fd_count-1 BEFORE exec_setup. The parent retains its own
// holds; this is "give the child its own ref," not "transfer."
//
// Lifetime: for each inherited fd, the handler takes an additional
// spoor_ref. Those bumped refs are owned by the spawn_args struct
// until the child thunk consumes them via handle_alloc. On rfork
// failure (or before rfork on validation failure), the handler drops
// all bumped refs.

struct spawn_with_fds_args {
    void          *blob;
    size_t         blob_size;
    u32            fd_count;
    struct Spoor  *spoors[SYS_SPAWN_MAX_FDS];
    // R15 F231 close: capture parent's slot rights at spawn time so
    // the child's handle_alloc preserves I-6 (rights monotonically
    // reduce on transfer). Without this capture, the child's handle
    // would get hardcoded RIGHT_READ|WRITE|TRANSFER regardless of the
    // parent's actual rights — a privilege elevation across the spawn
    // boundary that violates spec/handles.tla's RforkWithCaps
    // monotonicity expectation as extended for fd inheritance.
    rights_t       rights[SYS_SPAWN_MAX_FDS];
    // P5-corvus-srv-impl-b3a: SPAWN_PERM_* bits the spawn thunk applies
    // to the child Proc BEFORE exec_setup, atomically inside the new
    // Proc's first thread context. 0 for SYS_SPAWN_WITH_FDS / SYS_SPAWN_FULL;
    // SYS_SPAWN_WITH_PERMS carries the parent's vetted permission flags
    // here. See SPAWN_PERM_* in <thylacine/syscall.h>.
    u32            perm_flags;
};

__attribute__((noreturn))
static void sys_spawn_with_fds_thunk(void *arg) {
    struct spawn_with_fds_args *sa = (struct spawn_with_fds_args *)arg;
    void   *blob       = sa->blob;
    size_t  blob_size  = sa->blob_size;
    u32     fd_count   = sa->fd_count;
    u32     perm_flags = sa->perm_flags;
    struct Spoor *spoors_local[SYS_SPAWN_MAX_FDS];
    rights_t      rights_local[SYS_SPAWN_MAX_FDS];
    for (u32 i = 0; i < fd_count; i++) {
        spoors_local[i] = sa->spoors[i];
        rights_local[i] = sa->rights[i];
    }
    kfree(sa);

    struct Thread *t = current_thread();
    if (!t) extinction("sys_spawn_with_fds_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("sys_spawn_with_fds_thunk: no proc");

    // P5-corvus-srv-impl-b3a: apply parent-vetted SPAWN_PERM_* bits BEFORE
    // anything user-observable. Done here (in the child thread's context,
    // before exec_setup) rather than in the parent path so the child
    // never sees an un-stamped intermediate state — its very first user-
    // mode instruction observes the final proc_flags. The parent already
    // gate-checked every bit (sys_spawn_with_perms_for_proc); the thunk
    // just translates each bit to the kernel mark function. Fail-closed:
    // an unknown bit at this point is a kernel invariant violation (the
    // parent must have stripped it).
    if (perm_flags & SPAWN_PERM_MAY_POST_SERVICE) {
        proc_mark_may_post_service(p);
    }
    if (perm_flags & SPAWN_PERM_CONSOLE_TRUSTED) {
        proc_set_console_trusted(p);   // A-4c-2: the SAK re-grant target
    }
    if (perm_flags & ~SPAWN_PERM_ALL) {
        extinction("sys_spawn_with_fds_thunk: unknown SPAWN_PERM_* bit");
    }

    // Install each Spoor in the child's handle table at the lowest
    // free slot. Post-rfork, the table is empty, so the first install
    // is fd 0, then 1, then 2, etc. handle_alloc consumes the spoor_ref
    // we bumped on the parent's side; if it fails partway, the child's
    // proc_free → handle_table_free will release the installed handles,
    // and the un-installed spoors need explicit spoor_clunk.
    //
    // R15 F231 close: rights are inherited from the parent's slot, NOT
    // hardcoded. This preserves I-6 (rights monotonically reduce on
    // transfer): the child can never have rights the parent didn't
    // hold for the same Spoor.
    u32 installed = 0;
    for (u32 i = 0; i < fd_count; i++) {
        hidx_t fd = handle_alloc(p, KOBJ_SPOOR, rights_local[i],
                                 spoors_local[i]);
        if (fd != (hidx_t)i) {
            // Drop refs on the un-installed remainder; the installed
            // prefix gets cleaned up by proc_free.
            for (u32 j = i; j < fd_count; j++) spoor_clunk(spoors_local[j]);
            kfree(blob);
            exits("fail-fd-install");
        }
        installed++;
    }
    (void)installed;

    u64 entry = 0, sp = 0;
    int rc = exec_setup(p, blob, blob_size, &entry, &sp);
    kfree(blob);
    if (rc != 0) {
        // Installed handles cleaned by proc_free.
        exits("fail-exec");
    }

    userland_enter(entry, sp);
}

// Kernel-side body. Exported (non-static) for kernel-internal tests.
// fds is a kernel-resident array of fd_count entries; each must refer
// to an open KOBJ_SPOOR handle in `p`.
int sys_spawn_with_fds_for_proc(struct Proc *p, const char *name, size_t name_len,
                                const u32 *fds, u32 fd_count) {
    if (!p)                                            return -1;
    if (!name)                                         return -1;
    if (name_len == 0 || name_len > SYS_SPAWN_NAME_MAX) return -1;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\0')                            return -1;
    }
    if (name[name_len] != '\0')                         return -1;
    if (fd_count > SYS_SPAWN_MAX_FDS)                   return -1;
    if (fd_count > 0 && !fds)                           return -1;

    // Look up each fd, bump its spoor_ref, and capture the parent
    // slot's rights for the child-side install (R15 F231 close).
    struct Spoor *bumped[SYS_SPAWN_MAX_FDS];
    rights_t      bumped_rights[SYS_SPAWN_MAX_FDS];
    if (sys_bump_inherit_fds(p, fds, fd_count, bumped, bumped_rights) != 0)
        return -1;

    const void *cpio_blob = NULL;
    size_t      cpio_size = 0;
    if (devramfs_lookup(name, &cpio_blob, &cpio_size) != 0) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    if (cpio_size == 0 || cpio_size > SYS_SPAWN_BLOB_MAX) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

    void *blob_copy = kmalloc(cpio_size, 0);
    if (!blob_copy) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    if (((uintptr_t)blob_copy & 0x7) != 0) {
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    const u8 *src = (const u8 *)cpio_blob;
    u8 *dst = (u8 *)blob_copy;
    for (size_t i = 0; i < cpio_size; i++) dst[i] = src[i];

    struct spawn_with_fds_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    sa->blob      = blob_copy;
    sa->blob_size = cpio_size;
    sa->fd_count  = fd_count;
    for (u32 i = 0; i < fd_count; i++) {
        sa->spoors[i] = bumped[i];
        sa->rights[i] = bumped_rights[i];
    }

    int pid = rfork(RFPROC, sys_spawn_with_fds_thunk, sa);
    if (pid < 0) {
        kfree(sa);
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    return pid;
}

// =============================================================================
// SYS_SPAWN_WITH_CAPS — spawn with explicit cap_mask (P5-spawn-caps).
// =============================================================================
//
// Like SYS_SPAWN, but uses rfork_with_caps instead of rfork: the child's
// caps are `parent->caps & cap_mask`. ARCH I-2 / I-6 monotonicity is
// preserved structurally (the AND can only reduce, never elevate).
//
// Reuses the existing sys_spawn_thunk + spawn_args from SYS_SPAWN —
// the only difference is whether rfork or rfork_with_caps is used.

int sys_spawn_with_caps_for_proc(struct Proc *p, const char *name, size_t name_len,
                                 caps_t cap_mask) {
    if (!p)                                            return -1;
    if (!name)                                         return -1;
    if (name_len == 0 || name_len > SYS_SPAWN_NAME_MAX) return -1;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\0')                            return -1;
    }
    if (name[name_len] != '\0')                         return -1;

    const void *cpio_blob = NULL;
    size_t      cpio_size = 0;
    if (devramfs_lookup(name, &cpio_blob, &cpio_size) != 0) return -1;
    if (cpio_size == 0 || cpio_size > SYS_SPAWN_BLOB_MAX)   return -1;

    void *blob_copy = kmalloc(cpio_size, 0);
    if (!blob_copy)                                    return -1;
    if (((uintptr_t)blob_copy & 0x7) != 0) {
        kfree(blob_copy);
        return -1;
    }
    const u8 *src = (const u8 *)cpio_blob;
    u8 *dst = (u8 *)blob_copy;
    for (size_t i = 0; i < cpio_size; i++) dst[i] = src[i];

    struct spawn_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        kfree(blob_copy);
        return -1;
    }
    sa->blob      = blob_copy;
    sa->blob_size = cpio_size;

    int pid = rfork_with_caps(RFPROC, sys_spawn_thunk, sa, cap_mask);
    if (pid < 0) {
        kfree(sa);
        kfree(blob_copy);
        return -1;
    }
    return pid;
}

static s64 sys_spawn_with_caps_handler(u64 name_va, u64 name_len_raw, u64 cap_mask_raw) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    if (name_len_raw == 0 || name_len_raw > SYS_SPAWN_NAME_MAX) return -1;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;

    char name[SYS_SPAWN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b = 0;
        if (uaccess_load_u8(name_va + i, &b) != 0)     return -1;
        if (b == 0)                                    return -1;
        name[i] = (char)b;
    }
    name[name_len_raw] = '\0';

    return (s64)sys_spawn_with_caps_for_proc(p, name, (size_t)name_len_raw,
                                              (caps_t)cap_mask_raw);
}

// =============================================================================
// SYS_SPAWN_FULL — combined fds + caps (P5-spawn-full).
// =============================================================================
//
// Unions SYS_SPAWN_WITH_FDS (fd inheritance, KOBJ_SPOOR-only at v1.0) with
// SYS_SPAWN_WITH_CAPS (cap-subset via rfork_with_caps). Reuses the existing
// sys_spawn_with_fds_thunk + spawn_with_fds_args; only difference vs
// SYS_SPAWN_WITH_FDS is rfork_with_caps instead of rfork.
//
// Needed at P5-corvus-bringup where joey spawns /sbin/corvus with a
// pipe pair (login communication) AND CAP_LOCK_PAGES + CAP_CSPRNG_READ.
//
// SYS_SPAWN_WITH_PERMS (P5-corvus-srv-impl-b3a) extends this with a
// `perm_flags` parameter that the child's spawn thunk applies as one-way
// PROC_FLAG_* stamps BEFORE exec_setup. The shared implementation lives in
// `sys_spawn_full_with_perms_for_proc`; sys_spawn_full_for_proc + sys_spawn
// _with_perms_for_proc are thin wrappers that fix perm_flags to 0 or the
// caller's vetted bitmask respectively. Keeping one implementation avoids
// the per-variant drift the earlier copy-paste pattern accumulated.

// Internal: the unified spawn body. perm_flags MUST be vetted (callers
// gate-check console-attachment before passing nonzero bits).
static int sys_spawn_full_with_perms_for_proc(struct Proc *p,
                                              const char *name, size_t name_len,
                                              const u32 *fds, u32 fd_count,
                                              caps_t cap_mask, u32 perm_flags) {
    if (!p)                                            return -1;
    if (!name)                                         return -1;
    if (name_len == 0 || name_len > SYS_SPAWN_NAME_MAX) return -1;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\0')                            return -1;
    }
    if (name[name_len] != '\0')                         return -1;
    if (fd_count > SYS_SPAWN_MAX_FDS)                   return -1;
    if (fd_count > 0 && !fds)                           return -1;
    // perm_flags arrives already gate-checked by the public wrappers; the
    // bit-mask validation here is a defense-in-depth guard so a future
    // caller that bypasses the wrappers still fails closed on garbage bits.
    if (perm_flags & ~SPAWN_PERM_ALL)                   return -1;

    // Look up each fd, bump its spoor_ref, and capture the parent
    // slot's rights for the child-side install (R15 F231 close).
    struct Spoor *bumped[SYS_SPAWN_MAX_FDS];
    rights_t      bumped_rights[SYS_SPAWN_MAX_FDS];
    if (sys_bump_inherit_fds(p, fds, fd_count, bumped, bumped_rights) != 0)
        return -1;

    const void *cpio_blob = NULL;
    size_t      cpio_size = 0;
    if (devramfs_lookup(name, &cpio_blob, &cpio_size) != 0) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    if (cpio_size == 0 || cpio_size > SYS_SPAWN_BLOB_MAX) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

    void *blob_copy = kmalloc(cpio_size, 0);
    if (!blob_copy) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    if (((uintptr_t)blob_copy & 0x7) != 0) {
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    const u8 *src = (const u8 *)cpio_blob;
    u8 *dst = (u8 *)blob_copy;
    for (size_t i = 0; i < cpio_size; i++) dst[i] = src[i];

    struct spawn_with_fds_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    sa->blob       = blob_copy;
    sa->blob_size  = cpio_size;
    sa->fd_count   = fd_count;
    sa->perm_flags = perm_flags;
    for (u32 i = 0; i < fd_count; i++) {
        sa->spoors[i] = bumped[i];
        sa->rights[i] = bumped_rights[i];
    }

    int pid = rfork_with_caps(RFPROC, sys_spawn_with_fds_thunk, sa, cap_mask);
    if (pid < 0) {
        kfree(sa);
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    return pid;
}

int sys_spawn_full_for_proc(struct Proc *p, const char *name, size_t name_len,
                            const u32 *fds, u32 fd_count, caps_t cap_mask) {
    return sys_spawn_full_with_perms_for_proc(p, name, name_len, fds, fd_count,
                                              cap_mask, /*perm_flags=*/0u);
}

// SYS_SPAWN_WITH_PERMS — P5-corvus-srv-impl-b3a kernel body. Gates any
// nonzero SPAWN_PERM_* bit on the caller being console-attached (joey is
// the v1.0 console anchor; an ordinary Proc that wandered into this call
// has no path to confer the bit). Setting perm_flags=0 is identical to
// SYS_SPAWN_FULL — kept as a single entry point so callers that grant no
// permissions do not need a separate code path.
int sys_spawn_with_perms_for_proc(struct Proc *p,
                                  const char *name, size_t name_len,
                                  const u32 *fds, u32 fd_count,
                                  caps_t cap_mask, u32 perm_flags) {
    if (!p)                                             return -1;
    if (perm_flags & ~SPAWN_PERM_ALL)                   return -1;
    if (perm_flags != 0u && !proc_is_console_attached(p)) return -1;
    return sys_spawn_full_with_perms_for_proc(p, name, name_len, fds, fd_count,
                                              cap_mask, perm_flags);
}

static s64 sys_spawn_full_handler(u64 name_va, u64 name_len_raw,
                                  u64 fd_list_va, u64 fd_count_raw,
                                  u64 cap_mask_raw) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    if (name_len_raw == 0 || name_len_raw > SYS_SPAWN_NAME_MAX) return -1;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;
    if (fd_count_raw > SYS_SPAWN_MAX_FDS)               return -1;

    char name[SYS_SPAWN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b = 0;
        if (uaccess_load_u8(name_va + i, &b) != 0)     return -1;
        if (b == 0)                                    return -1;
        name[i] = (char)b;
    }
    name[name_len_raw] = '\0';

    u32 fds_kbuf[SYS_SPAWN_MAX_FDS] = { 0 };
    if (fd_count_raw > 0) {
        u64 list_bytes = fd_count_raw * sizeof(u32);
        if (!sys_validate_user_buf(fd_list_va, list_bytes)) return -1;
        for (u64 i = 0; i < fd_count_raw; i++) {
            u32 v = 0;
            for (u64 b = 0; b < sizeof(u32); b++) {
                u8 byte = 0;
                if (uaccess_load_u8(fd_list_va + i * sizeof(u32) + b, &byte) != 0)
                    return -1;
                v |= (u32)byte << (b * 8);
            }
            fds_kbuf[i] = v;
        }
    }

    return (s64)sys_spawn_full_for_proc(p, name, (size_t)name_len_raw,
                                        fds_kbuf, (u32)fd_count_raw,
                                        (caps_t)cap_mask_raw);
}

// SYS_SPAWN_WITH_PERMS handler — same shape as sys_spawn_full_handler but
// reads a sixth argument (perm_flags) and routes to the perms-bearing core.
// Sharing the parser body via copy-and-extend rather than an internal
// helper keeps the user-VA validation visibly local to each entry point
// (every syscall handler reads its own args under the same audit lens).
static s64 sys_spawn_with_perms_handler(u64 name_va, u64 name_len_raw,
                                        u64 fd_list_va, u64 fd_count_raw,
                                        u64 cap_mask_raw, u64 perm_flags_raw) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    // perm_flags is a u32 over the wire; reject any bits past the
    // documented SPAWN_PERM_* set BEFORE any user-VA copy so a hostile
    // caller cannot probe for fault behavior with high bits set.
    if (perm_flags_raw & ~(u64)SPAWN_PERM_ALL)         return -1;

    if (name_len_raw == 0 || name_len_raw > SYS_SPAWN_NAME_MAX) return -1;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;
    if (fd_count_raw > SYS_SPAWN_MAX_FDS)               return -1;

    char name[SYS_SPAWN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b = 0;
        if (uaccess_load_u8(name_va + i, &b) != 0)     return -1;
        if (b == 0)                                    return -1;
        name[i] = (char)b;
    }
    name[name_len_raw] = '\0';

    u32 fds_kbuf[SYS_SPAWN_MAX_FDS] = { 0 };
    if (fd_count_raw > 0) {
        u64 list_bytes = fd_count_raw * sizeof(u32);
        if (!sys_validate_user_buf(fd_list_va, list_bytes)) return -1;
        for (u64 i = 0; i < fd_count_raw; i++) {
            u32 v = 0;
            for (u64 b = 0; b < sizeof(u32); b++) {
                u8 byte = 0;
                if (uaccess_load_u8(fd_list_va + i * sizeof(u32) + b, &byte) != 0)
                    return -1;
                v |= (u32)byte << (b * 8);
            }
            fds_kbuf[i] = v;
        }
    }

    return (s64)sys_spawn_with_perms_for_proc(p, name, (size_t)name_len_raw,
                                              fds_kbuf, (u32)fd_count_raw,
                                              (caps_t)cap_mask_raw,
                                              (u32)perm_flags_raw);
}

// =============================================================================
// SYS_SPAWN_FULL_ARGV — argv pass-through spawn (P6-pouch-stratumd-boot 16b-a).
// =============================================================================
//
// Combined spawn primitive that extends SYS_SPAWN_WITH_PERMS with argv
// pass-through. Stratumd in sub-chunk 16b-beta needs a real argv (pool
// path + --keyfile + --listen + ...); the legacy SYS_SPAWN_* family
// inherits only argv=[name]. Rather than adding yet another register-
// based permutation (the WITH_PERMS handler is already at the 6-arg
// register-ABI ceiling), the new entry takes a single user pointer to a
// struct sys_spawn_args carrying every existing spawn feature plus the
// new argv fields.
//
// Lifetime: the argv buffer is uaccess-copied into a kernel kmalloc'd
// region BEFORE rfork. The spawn_full_argv_args struct OWNS the argv copy
// until the thunk consumes it via exec_setup_with_argv. The user-side
// buffer is never observed post-syscall. argv strings carry no handles
// (I-4 + I-5 structurally upheld).
//
// Validation invariants (all -1 on violation):
//   - sys_validate_user_buf on req_va for sizeof(struct sys_spawn_args).
//   - name_len in [1, SYS_SPAWN_NAME_MAX]; name bytes non-NUL.
//   - argv_data_len in [0, SYS_SPAWN_ARGV_DATA_MAX]; if argc == 0 then
//     argv_data_len == 0; if argc > 0 then argv_data_len > 0 AND the
//     last byte is NUL AND the NUL count == argc.
//   - argc in [0, SYS_SPAWN_ARGV_MAX].
//   - fd_count in [0, SYS_SPAWN_MAX_FDS]; each fd a live KOBJ_SPOOR.
//   - perm_flags subset of SPAWN_PERM_ALL; nonzero only if console-attached.
//   - _pad_envp == 0 (reserved for forward-compat envp pass-through;
//     reject non-zero values so a future envp wiring cannot silently land
//     on a v1.0 kernel).

// A-1a: identity bundle threaded from the spawn handler/entry into the
// child via spawn_full_argv_args. `set` mirrors SPAWN_IDENTITY_SET; when
// false the child INHERITS (proc_apply_identity is not called and rfork's
// inherit stands). docs/IDENTITY-DESIGN.md §9.1.
struct spawn_identity {
    bool set;
    u32  principal_id;
    u32  primary_gid;
    u32  supp_gids[PROC_SUPP_GIDS_MAX];
    u8   supp_gid_count;
};

// A settable id is legitimate iff it is a real corvus-assignable value
// ([1, 0xFFFFFFFD]) OR the NONE sentinel. INVALID(0) and the SYSTEM
// sentinel are rejected — you cannot stamp the never-valid 0 nor forge the
// system identity via the spawn path (I-22). The gid reserved scheme
// shares values with the principal scheme (INVALID/SYSTEM/NONE), so one
// predicate serves both. corvus is the authority for WHICH real ids a
// login may request; the kernel sanity-bounds only.
static bool spawn_identity_id_ok(u32 id) {
    return id == PRINCIPAL_NONE ||
           (id != PRINCIPAL_INVALID && id != PRINCIPAL_SYSTEM);
}

static bool spawn_identity_value_ok(const struct spawn_identity *id) {
    if (!id)                                      return false;
    if (!spawn_identity_id_ok(id->principal_id))  return false;
    if (!spawn_identity_id_ok(id->primary_gid))   return false;
    if (id->supp_gid_count > PROC_SUPP_GIDS_MAX)  return false;
    // A-1a R1 F1: reject INVALID *and* SYSTEM on supplementary gids too — the
    // same predicate as the primary id/gid. An asymmetry (primary rejects
    // SYSTEM, supp only rejected 0) would let a capped login smuggle the
    // system group into a user's supplementary set, which becomes authority
    // once A-2d enforces group rwx (I-22).
    for (u8 i = 0; i < id->supp_gid_count; i++) {
        if (!spawn_identity_id_ok(id->supp_gids[i])) return false;
    }
    return true;
}

// Kernel-side spawn args for SYS_SPAWN_FULL_ARGV. Mirrors
// spawn_with_fds_args but adds argv_data ownership.
struct spawn_full_argv_args {
    void          *blob;
    size_t         blob_size;
    u32            fd_count;
    struct Spoor  *spoors[SYS_SPAWN_MAX_FDS];
    // R15 F231: rights captured at spawn time so the child's install
    // preserves I-6 (rights monotonically reduce on transfer).
    rights_t       rights[SYS_SPAWN_MAX_FDS];
    u32            perm_flags;
    // argv ownership — kmalloc'd kernel buffer; the thunk passes it to
    // exec_setup_with_argv, then kfree's at the END (only after the
    // user-frame copy completes). Lifetime ends at the thunk; never
    // crosses any Proc boundary.
    char          *argv_data;
    u32            argv_data_len;
    u32            argc;
    // A-1a: optional identity override (applied in the thunk before exec
    // when identity.set; else the child keeps rfork's inherited identity).
    struct spawn_identity identity;
};

__attribute__((noreturn))
static void sys_spawn_full_argv_thunk(void *arg) {
    struct spawn_full_argv_args *sa = (struct spawn_full_argv_args *)arg;
    void   *blob          = sa->blob;
    size_t  blob_size     = sa->blob_size;
    u32     fd_count      = sa->fd_count;
    u32     perm_flags    = sa->perm_flags;
    char   *argv_data     = sa->argv_data;
    u32     argv_data_len = sa->argv_data_len;
    u32     argc          = sa->argc;
    struct spawn_identity identity = sa->identity;   // A-1a: copy before kfree
    struct Spoor *spoors_local[SYS_SPAWN_MAX_FDS];
    rights_t      rights_local[SYS_SPAWN_MAX_FDS];
    for (u32 i = 0; i < fd_count; i++) {
        spoors_local[i] = sa->spoors[i];
        rights_local[i] = sa->rights[i];
    }
    kfree(sa);

    struct Thread *t = current_thread();
    if (!t) extinction("sys_spawn_full_argv_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("sys_spawn_full_argv_thunk: no proc");

    // Apply parent-vetted SPAWN_PERM_* bits BEFORE anything user-
    // observable; the parent gate-checked the bits in sys_spawn_full_
    // argv_for_proc (R1 F5 fix: was incorrectly attributed to sys_spawn_
    // with_perms_for_proc). Same pattern as sys_spawn_with_fds_thunk's
    // perm-application block.
    if (perm_flags & SPAWN_PERM_MAY_POST_SERVICE) {
        proc_mark_may_post_service(p);
    }
    if (perm_flags & SPAWN_PERM_CONSOLE_TRUSTED) {
        proc_set_console_trusted(p);   // A-4c-2: the SAK re-grant target
    }
    if (perm_flags & ~SPAWN_PERM_ALL) {
        extinction("sys_spawn_full_argv_thunk: unknown SPAWN_PERM_* bit");
    }

    // A-1a: apply the parent-vetted identity override BEFORE any user-
    // observable state (fd install / exec / userland_enter). The parent
    // verified CAP_SET_IDENTITY + value bounds in
    // sys_spawn_full_argv_identity_for_proc; here we just stamp. When
    // identity.set is false the child keeps the identity rfork inherited.
    // This is what makes "set at creation" hold: the child never runs EL0
    // under the wrong identity. docs/IDENTITY-DESIGN.md §9.1.
    if (identity.set) {
        proc_apply_identity(p, identity.principal_id, identity.primary_gid,
                            identity.supp_gids, identity.supp_gid_count);
    }

    // Install inherited fds (same pattern as sys_spawn_with_fds_thunk).
    u32 installed = 0;
    for (u32 i = 0; i < fd_count; i++) {
        hidx_t fd = handle_alloc(p, KOBJ_SPOOR, rights_local[i],
                                 spoors_local[i]);
        if (fd != (hidx_t)i) {
            for (u32 j = i; j < fd_count; j++) spoor_clunk(spoors_local[j]);
            kfree(blob);
            kfree(argv_data);
            exits("fail-fd-install");
        }
        installed++;
    }
    (void)installed;

    u64 entry = 0, sp = 0;
    int rc = exec_setup_with_argv(p, blob, blob_size,
                                  argv_data, argv_data_len, argc,
                                  &entry, &sp);
    kfree(blob);
    kfree(argv_data);
    if (rc != 0) {
        exits("fail-exec");
    }

    userland_enter(entry, sp);
}

// Internal: the unified spawn-with-argv body. Mirrors sys_spawn_full_with_
// perms_for_proc but threads argv through; perm_flags MUST be vetted by
// the caller (gate-checks console-attachment before passing nonzero).
static int sys_spawn_full_argv_with_perms_for_proc(
        struct Proc *p,
        const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        const struct spawn_identity *id) {
    if (!p)                                            return -1;
    if (!name)                                         return -1;
    if (name_len == 0 || name_len > SYS_SPAWN_NAME_MAX) return -1;
    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '\0')                            return -1;
    }
    if (name[name_len] != '\0')                         return -1;
    if (fd_count > SYS_SPAWN_MAX_FDS)                   return -1;
    if (fd_count > 0 && !fds)                           return -1;
    if (perm_flags & ~SPAWN_PERM_ALL)                   return -1;

    // argv validation. Both shapes accepted: (argc=0, argv_data_len=0,
    // argv_data=NULL) is the "no argv" case (equivalent to legacy
    // SYS_SPAWN_WITH_PERMS); (argc>0) requires a NUL-terminated buffer
    // with exactly argc NULs.
    if (argc > SYS_SPAWN_ARGV_MAX)                     return -1;
    if (argv_data_len > SYS_SPAWN_ARGV_DATA_MAX)       return -1;
    if (argc == 0) {
        if (argv_data_len != 0)                        return -1;
    } else {
        if (argv_data_len == 0)                        return -1;
        if (!argv_data)                                return -1;
        if (argv_data[argv_data_len - 1] != '\0')      return -1;
        u32 nuls = 0;
        for (u32 i = 0; i < argv_data_len; i++) {
            if (argv_data[i] == '\0') nuls++;
        }
        if (nuls != argc)                              return -1;
    }

    // Bump fds (same pattern as sys_spawn_with_fds_for_proc).
    struct Spoor *bumped[SYS_SPAWN_MAX_FDS];
    rights_t      bumped_rights[SYS_SPAWN_MAX_FDS];
    if (sys_bump_inherit_fds(p, fds, fd_count, bumped, bumped_rights) != 0)
        return -1;

    const void *cpio_blob = NULL;
    size_t      cpio_size = 0;
    if (devramfs_lookup(name, &cpio_blob, &cpio_size) != 0) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    if (cpio_size == 0 || cpio_size > SYS_SPAWN_BLOB_MAX) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

    void *blob_copy = kmalloc(cpio_size, 0);
    if (!blob_copy) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    if (((uintptr_t)blob_copy & 0x7) != 0) {
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    {
        const u8 *src = (const u8 *)cpio_blob;
        u8 *dst = (u8 *)blob_copy;
        for (size_t i = 0; i < cpio_size; i++) dst[i] = src[i];
    }

    // Kernel-side argv copy. Lifetime: owned by the spawn_args struct
    // until the thunk's exec_setup_with_argv consumes it. Free-on-error
    // paths handle every interleaving below.
    char *argv_data_copy = NULL;
    if (argv_data_len > 0) {
        argv_data_copy = kmalloc(argv_data_len, 0);
        if (!argv_data_copy) {
            kfree(blob_copy);
            for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
            return -1;
        }
        for (u32 i = 0; i < argv_data_len; i++) argv_data_copy[i] = argv_data[i];
    }

    struct spawn_full_argv_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        if (argv_data_copy) kfree(argv_data_copy);
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    sa->blob          = blob_copy;
    sa->blob_size     = cpio_size;
    sa->fd_count      = fd_count;
    sa->perm_flags    = perm_flags;
    sa->argv_data     = argv_data_copy;
    sa->argv_data_len = argv_data_len;
    sa->argc          = argc;
    // A-1a: carry the identity override (KP_ZERO already left identity.set
    // false, so a NULL `id` means inherit). The parent already gated +
    // validated it; the thunk stamps it before exec.
    if (id) sa->identity = *id;
    for (u32 i = 0; i < fd_count; i++) {
        sa->spoors[i] = bumped[i];
        sa->rights[i] = bumped_rights[i];
    }

    int pid = rfork_with_caps(RFPROC, sys_spawn_full_argv_thunk, sa, cap_mask);
    if (pid < 0) {
        kfree(sa);
        if (argv_data_copy) kfree(argv_data_copy);
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    return pid;
}

// A-1a: identity-aware entry — the real gate site. Does the console-perm
// gate AND the CAP_SET_IDENTITY gate (FAIL-CLOSED) + reserved-value reject,
// then delegates to the body. Exported (non-static) for kernel tests; the
// identity is passed as scalars (not the internal struct spawn_identity)
// so the test file needs no kernel-internal type. set_identity == false
// (the back-compat path) means the child inherits the parent's identity.
int sys_spawn_full_argv_identity_for_proc(struct Proc *p,
        const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        bool set_identity, u32 principal_id, u32 primary_gid,
        const u32 *supp_gids, u32 supp_gid_count) {
    if (!p)                                             return -1;
    if (perm_flags & ~SPAWN_PERM_ALL)                   return -1;
    if (perm_flags != 0u && !proc_is_console_attached(p)) return -1;

    struct spawn_identity id = {0};
    const struct spawn_identity *eff_id = NULL;
    if (set_identity) {
        // FAIL-CLOSED cap gate (caps read under acquire, matching
        // rfork_with_caps): a SET request without CAP_SET_IDENTITY is
        // rejected with -1, never silently downgraded to inherit. I-22:
        // this gate touches only the IDENTITY axis; cap_mask still governs
        // the child's caps independently.
        caps_t my_caps = __atomic_load_n(&p->caps, __ATOMIC_ACQUIRE);
        if (!(my_caps & CAP_SET_IDENTITY))             return -1;
        // Bound the count BEFORE copying into the fixed-size bundle.
        if (supp_gid_count > PROC_SUPP_GIDS_MAX)       return -1;
        id.set            = true;
        id.principal_id   = principal_id;
        id.primary_gid    = primary_gid;
        id.supp_gid_count = (u8)supp_gid_count;
        for (u32 i = 0; i < supp_gid_count; i++)
            id.supp_gids[i] = supp_gids ? supp_gids[i] : 0u;
        // Reserved-value reject (INVALID/SYSTEM ids; INVALID supp gids).
        if (!spawn_identity_value_ok(&id))             return -1;
        eff_id = &id;
    }

    return sys_spawn_full_argv_with_perms_for_proc(p, name, name_len,
                                                   argv_data, argv_data_len,
                                                   argc, fds, fd_count,
                                                   cap_mask, perm_flags, eff_id);
}

// Back-compat entry: inherit identity (no SET). Unchanged signature for
// existing callers + the SYS_SPAWN_WITH_PERMS-shaped tests. perm_flags
// console-attachment gate is enforced by the identity entry above.
int sys_spawn_full_argv_for_proc(struct Proc *p,
                                 const char *name, size_t name_len,
                                 const char *argv_data, u32 argv_data_len,
                                 u32 argc,
                                 const u32 *fds, u32 fd_count,
                                 caps_t cap_mask, u32 perm_flags) {
    return sys_spawn_full_argv_identity_for_proc(p, name, name_len,
                                                 argv_data, argv_data_len, argc,
                                                 fds, fd_count, cap_mask,
                                                 perm_flags,
                                                 /*set_identity=*/false,
                                                 PRINCIPAL_INVALID, GID_INVALID,
                                                 NULL, 0u);
}

// uaccess-loader helper: copy the struct sys_spawn_args from user memory
// in a single pass. The struct is 80 bytes (pinned by _Static_assert in
// the ABI header); we read it byte-by-byte to avoid pointer-cast
// strict-aliasing pitfalls and to keep every load on the uaccess fixup
// path. Returns 0 on success, -1 on any uaccess fault.
static int sys_load_spawn_args(u64 req_va, struct sys_spawn_args *out) {
    u8 *dst = (u8 *)out;
    for (u64 i = 0; i < sizeof(*out); i++) {
        u8 b = 0;
        if (uaccess_load_u8(req_va + i, &b) != 0) return -1;
        dst[i] = b;
    }
    return 0;
}

// R1 F1 fix: handler-side field-bound validation extracted as a
// kernel-internal helper so kernel tests can exercise the handler's
// distinctive checks (_pad_envp != 0, perm_flags & ~ALL, oversize fields)
// without needing an SVC instruction or a user-VA fixture. Returns 0 on
// "all fields pass static bounds", -1 on any violation.
//
// This helper deliberately does NOT do any uaccess work: callers (the
// handler) do uaccess BEFORE calling this so a fault returns -1 distinct
// from a field-bounds violation. The argc/argv_data_len symmetry check
// (argc == 0 ⟺ argv_data_len == 0) is also enforced here; the body
// re-checks defense-in-depth on the same invariant.
int sys_spawn_full_argv_validate_req(const struct sys_spawn_args *req) {
    if (!req)                                          return -1;
    if (req->name_len == 0 || req->name_len > SYS_SPAWN_NAME_MAX) return -1;
    if (req->argv_data_len > SYS_SPAWN_ARGV_DATA_MAX)  return -1;
    if (req->argc > SYS_SPAWN_ARGV_MAX)                return -1;
    if (req->fd_count > SYS_SPAWN_MAX_FDS)              return -1;
    if (req->perm_flags & ~(u32)SPAWN_PERM_ALL)         return -1;
    if (req->_pad_envp != 0)                           return -1;
    // R1 F4 fix: reject (argc > 0, argv_data_len == 0) at the handler's
    // field-bound stage rather than waiting for the body's NUL-walk to
    // reject it. Symmetric to the existing (argc == 0, argv_data_len > 0)
    // check and saves the uaccess sub-buffer copies on a guaranteed-fail
    // input.
    if (req->argc > 0 && req->argv_data_len == 0)      return -1;
    if (req->argc == 0 && req->argv_data_len != 0)     return -1;
    // A-1a: identity_flags must carry no unknown bits (forward-compat — a
    // future flag cannot silently land on a v1.0 kernel; same rationale as
    // _pad_envp). When SPAWN_IDENTITY_SET is set, bound supp_gid_count here
    // (the handler's supp-gid copy loop indexes a PROC_SUPP_GIDS_MAX buffer;
    // the identity entry re-checks defense-in-depth). The id VALUE checks
    // (reserved-reject) live in the identity entry AFTER the cap gate, so an
    // uncapped caller learns only "rejected", never which value was bad.
    if (req->identity_flags & ~(u32)SPAWN_IDENTITY_FLAGS_ALL) return -1;
    if ((req->identity_flags & SPAWN_IDENTITY_SET) &&
        req->supp_gid_count > PROC_SUPP_GIDS_MAX)      return -1;
    return 0;
}

static s64 sys_spawn_full_argv_handler(u64 req_va) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    // Validate + load the struct.
    if (!sys_validate_user_buf(req_va, sizeof(struct sys_spawn_args)))
        return -1;
    struct sys_spawn_args req;
    if (sys_load_spawn_args(req_va, &req) != 0)        return -1;

    // Field bounds (refuse oversized inputs BEFORE allocating any kernel
    // memory or copying any buffer). Extracted as a helper for kernel-
    // test coverage; see sys_spawn_full_argv_validate_req above.
    if (sys_spawn_full_argv_validate_req(&req) != 0)   return -1;

    // Sub-buffer validity (each pointer + its length).
    if (!sys_validate_user_buf(req.name_va, req.name_len)) return -1;
    if (req.argv_data_len > 0 &&
        !sys_validate_user_buf(req.argv_data_va, req.argv_data_len))
        return -1;
    if (req.fd_count > 0) {
        u64 fd_bytes = (u64)req.fd_count * sizeof(u32);
        if (!sys_validate_user_buf(req.fd_list_va, fd_bytes)) return -1;
    }

    // Copy name (NUL-rejection inline, matches existing handlers).
    char name[SYS_SPAWN_NAME_MAX + 1];
    for (u32 i = 0; i < req.name_len; i++) {
        u8 b = 0;
        if (uaccess_load_u8(req.name_va + i, &b) != 0) return -1;
        if (b == 0)                                    return -1;
        name[i] = (char)b;
    }
    name[req.name_len] = '\0';

    // Copy fd list (matches existing handlers' byte-by-byte pattern).
    u32 fds_kbuf[SYS_SPAWN_MAX_FDS] = { 0 };
    for (u32 i = 0; i < req.fd_count; i++) {
        u32 v = 0;
        for (u32 b = 0; b < sizeof(u32); b++) {
            u8 byte = 0;
            if (uaccess_load_u8(req.fd_list_va + i * sizeof(u32) + b, &byte) != 0)
                return -1;
            v |= (u32)byte << (b * 8);
        }
        fds_kbuf[i] = v;
    }

    // Copy argv_data into a stack buffer for in-kernel validation. The
    // body re-copies into a kmalloc'd region before rfork; the stack
    // buffer here only lives for the duration of the handler and the
    // synchronous body call below. Bound: SYS_SPAWN_ARGV_DATA_MAX = 4 KiB.
    char argv_kbuf[SYS_SPAWN_ARGV_DATA_MAX];
    for (u32 i = 0; i < req.argv_data_len; i++) {
        u8 b = 0;
        if (uaccess_load_u8(req.argv_data_va + i, &b) != 0) return -1;
        argv_kbuf[i] = (char)b;
    }

    // A-1a: copy supplementary gids only when a SET identity is requested.
    // validate_req already bounded supp_gid_count <= PROC_SUPP_GIDS_MAX for
    // a SET request; the explicit re-check here guards the supp_kbuf write
    // loop against a future reordering of the validation. The CAP_SET_IDENTITY
    // gate + reserved-value reject run later in the identity entry.
    u32  supp_kbuf[PROC_SUPP_GIDS_MAX] = { 0 };
    bool set_identity = (req.identity_flags & SPAWN_IDENTITY_SET) != 0;
    u32  supp_count   = 0;
    if (set_identity) {
        supp_count = req.supp_gid_count;
        if (supp_count > PROC_SUPP_GIDS_MAX)              return -1;
        if (supp_count > 0) {
            u64 supp_bytes = (u64)supp_count * sizeof(u32);
            if (!sys_validate_user_buf(req.supp_gids_va, supp_bytes)) return -1;
            for (u32 i = 0; i < supp_count; i++) {
                u32 v = 0;
                for (u32 b = 0; b < sizeof(u32); b++) {
                    u8 byte = 0;
                    if (uaccess_load_u8(req.supp_gids_va + i * sizeof(u32) + b,
                                        &byte) != 0)
                        return -1;
                    v |= (u32)byte << (b * 8);
                }
                supp_kbuf[i] = v;
            }
        }
    }

    return (s64)sys_spawn_full_argv_identity_for_proc(
        p, name, (size_t)req.name_len,
        req.argv_data_len > 0 ? argv_kbuf : NULL, req.argv_data_len, req.argc,
        fds_kbuf, req.fd_count,
        (caps_t)req.cap_mask, req.perm_flags,
        set_identity, req.principal_id, req.primary_gid,
        supp_kbuf, supp_count);
}

static s64 sys_spawn_with_fds_handler(u64 name_va, u64 name_len_raw,
                                      u64 fd_list_va, u64 fd_count_raw) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    if (name_len_raw == 0 || name_len_raw > SYS_SPAWN_NAME_MAX) return -1;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;
    if (fd_count_raw > SYS_SPAWN_MAX_FDS)               return -1;

    char name[SYS_SPAWN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b = 0;
        if (uaccess_load_u8(name_va + i, &b) != 0)     return -1;
        if (b == 0)                                    return -1;
        name[i] = (char)b;
    }
    name[name_len_raw] = '\0';

    u32 fds_kbuf[SYS_SPAWN_MAX_FDS] = { 0 };
    if (fd_count_raw > 0) {
        u64 list_bytes = fd_count_raw * sizeof(u32);
        if (!sys_validate_user_buf(fd_list_va, list_bytes)) return -1;
        for (u64 i = 0; i < fd_count_raw; i++) {
            u32 v = 0;
            for (u64 b = 0; b < sizeof(u32); b++) {
                u8 byte = 0;
                if (uaccess_load_u8(fd_list_va + i * sizeof(u32) + b, &byte) != 0)
                    return -1;
                v |= (u32)byte << (b * 8);
            }
            fds_kbuf[i] = v;
        }
    }

    return (s64)sys_spawn_with_fds_for_proc(p, name, (size_t)name_len_raw,
                                            fds_kbuf, (u32)fd_count_raw);
}

// =============================================================================
// SYS_WAIT_PID — reap one ZOMBIE child (P5-spawn-wait).
// =============================================================================
//
// ABI: x0 = status_out_va (0 to skip) → reaped_pid / -1.
//
// Thin wrapper over kernel/proc.c::wait_pid. The kernel side blocks if
// the caller has live but not-yet-zombie children, returns -1 if no
// children at all, and reaps + returns the PID on a successful wait.
//
// On success, writes the child's exit_status (sizeof(int) = 4 bytes)
// via per-byte uaccess_store_u8 if status_out_va is non-zero.

static s64 sys_wait_pid_handler(u64 status_out_va) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    // Validate the destination buffer up-front (skipping if NULL) so a
    // bad user-VA doesn't cause a reap-then-fault hazard.
    if (status_out_va != 0) {
        if (!sys_validate_user_buf(status_out_va, sizeof(int))) return -1;
    }

    int status = 0;
    int reaped = wait_pid(&status);
    if (reaped < 0)                                    return -1;

    if (status_out_va != 0) {
        // exit_status is int (4 bytes); host endianness on AArch64 is LE.
        const u8 *bytes = (const u8 *)&status;
        for (u64 i = 0; i < sizeof(int); i++) {
            if (uaccess_store_u8(status_out_va + i, bytes[i]) != 0) {
                // F240: status write failed after reap; the child is
                // gone but partial bytes may be visible to userspace.
                // Best-effort scrub the partial range to zero so the
                // caller doesn't read torn status (uaccess_store_u8 of
                // zero may itself fail; ignore — the caller saw -1 and
                // must not trust the status buffer either way).
                for (u64 j = 0; j < i; j++) {
                    (void)uaccess_store_u8(status_out_va + j, 0);
                }
                return -1;
            }
        }
    }

    return (s64)reaped;
}

// =============================================================================
// SYS_POST_SERVICE / SYS_POST_SERVICE_BYTE — register the caller as a /srv
// service (P5-corvus-srv; P6-pouch-sockets sub-chunk 12 added byte mode).
// =============================================================================
//
// Registers the calling Proc as the server for /srv/<name> and returns a
// KObj_Srv service handle. CORVUS-DESIGN.md §6.1 + POUCH-DESIGN.md §6.2;
// spec contract specs/corvus.tla::PostService (the marked-poster gate +
// the reserve/commit two-phase).
//
// Three entry points share one core:
//   - sys_post_service_for_proc(p, name, len) — the 3-arg legacy path
//     (default SRV_MODE_9P); kernel tests + corvus depend on this
//     signature unchanged.
//   - sys_post_service_byte_for_proc(p, name, len) — the byte-mode
//     variant (SRV_MODE_BYTE); pouch's AF_UNIX bind() uses this.
//   - sys_post_service_core(p, name, len, mode) — the shared
//     implementation. NOT exposed to tests.
//
// Returns the service handle (hidx ≥ 0) on success, -1 on failure.
static int sys_post_service_core(struct Proc *p, const char *name,
                                  size_t name_len, enum srv_mode mode) {
    if (!p)                                            return -1;
    if (!name)                                         return -1;
    if (name_len == 0 || name_len > SRV_NAME_MAX)       return -1;
    if (mode != SRV_MODE_9P && mode != SRV_MODE_BYTE)   return -1;

    // Post-gate (corvus.tla PostService precondition `p \in service_-
    // marked`): the caller must carry the one-way joey-stamped bit.
    // Fail-closed — proc_may_post_service returns false for a bad Proc.
    if (!proc_may_post_service(p))                      return -1;

    // Service names are short identifiers: printable ASCII, no '/' (a
    // path separator the P5-corvus-srv-impl-a3 walk splits on), no
    // control bytes. Reject anything else so a name can never be
    // mis-parsed once it is a /srv path component.
    for (size_t i = 0; i < name_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x21u || c > 0x7eu || c == '/')         return -1;
    }

    // Phase 1: reserve a registry slot. The entry goes RESERVING — it is
    // never observably LIVE until the handle below exists.
    struct SrvService *svc = NULL;
    enum srv_state     prior = SRV_STATE_FREE;
    if (srv_reserve(name, (u8)name_len, p, mode, &svc, &prior) != 0) return -1;

    // Install the KObj_Srv service handle. The handle's obj is the
    // registry entry; the entry outlives the handle (its lifetime is the
    // poster Proc's, via the exits() tombstone), so handle close does not
    // free it — handle_release_obj's KOBJ_SRV case is a no-op for a
    // service object.
    hidx_t h = handle_alloc(p, KOBJ_SRV, RIGHT_READ | RIGHT_WRITE, svc);
    if (h < 0) {
        // Phase 2 (failure): roll the reservation back to its prior state
        // (FREE for a fresh post, TOMBSTONED for a rebind) so a retry —
        // or another poster — can still claim the name.
        srv_abort(svc, prior);
        return -1;
    }

    // Phase 2 (success): RESERVING -> LIVE. Infallible.
    srv_commit(svc);
    return (int)h;
}

// Public 3-arg wrapper — the signature kernel tests + the existing
// SYS_POST_SERVICE handler call. SRV_MODE_9P is the historical default
// (the only mode pre-P6-pouch-sockets).
int sys_post_service_for_proc(struct Proc *p, const char *name,
                              size_t name_len) {
    return sys_post_service_core(p, name, name_len, SRV_MODE_9P);
}

// Public 3-arg byte-mode wrapper — used by the SYS_POST_SERVICE_BYTE
// handler. Same shape; SRV_MODE_BYTE flips the SrvService transport.
int sys_post_service_byte_for_proc(struct Proc *p, const char *name,
                                    size_t name_len) {
    return sys_post_service_core(p, name, name_len, SRV_MODE_BYTE);
}

// post_service_common — shared handler body for SYS_POST_SERVICE and
// SYS_POST_SERVICE_BYTE. Forks only on the `mode` arg passed through to
// sys_post_service_core. All other validation (length, user-VA,
// per-byte copy) is identical.
static s64 sys_post_service_common(u64 name_va, u64 name_len_raw,
                                    enum srv_mode mode) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    if (name_len_raw == 0 || name_len_raw > SRV_NAME_MAX) return -1;

    // F6 close (P5-corvus-srv-impl audit): validate the full user-VA
    // range BEFORE the per-byte copy, matching every other syscall
    // surface that takes a user buffer (sys_spawn_*, sys_srv_peer,
    // sys_srv_connect). The per-byte uaccess_load_u8 IS fault-safe
    // (returns -1 on a bad address), but a pre-validated range fails
    // a bad name_va before any byte is read into kernel scratch —
    // matches the pattern documented in sys_validate_user_buf.
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;

    // Copy the name from user-VA into kernel scratch, per-byte
    // uaccess_load_u8 (same shape as SYS_SPAWN's name copy). name_len_raw
    // ≤ SRV_NAME_MAX is the buffer bound.
    char name[SRV_NAME_MAX];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(name_va + i, &b) != 0)      return -1;
        name[i] = (char)b;
    }

    return (s64)sys_post_service_core(p, name, (size_t)name_len_raw, mode);
}

static s64 sys_post_service_handler(u64 name_va, u64 name_len_raw) {
    return sys_post_service_common(name_va, name_len_raw, SRV_MODE_9P);
}

static s64 sys_post_service_byte_handler(u64 name_va, u64 name_len_raw) {
    return sys_post_service_common(name_va, name_len_raw, SRV_MODE_BYTE);
}

// =============================================================================
// SYS_SRV_ACCEPT — accept a kernel-minted /srv connection (P5-corvus-srv).
// =============================================================================
//
// The poster of a /srv service blocks here until a client opens the
// service, then receives the server endpoint of one fresh kernel-minted
// connection as a KObj_Spoor handle (CORVUS-DESIGN.md §6.2; spec contract
// specs/corvus.tla::SrvAccept — corvus accepts only a connection the
// kernel already bound). The connection transport is kernel-created and
// kernel-owned throughout (invariant C-23).
//
// Returns the connection handle (hidx ≥ 0) on success, -1 on failure.
int sys_srv_accept_for_proc(struct Proc *p, hidx_t service_h) {
    if (!p) return -1;

    // Resolve the service handle: a KObj_Srv handle the caller holds whose
    // obj is a service registry entry. The first u64 discriminates a
    // service object (SRV_SERVICE_MAGIC) from a connection object
    // (SRV_CONN_MAGIC) — accept requires a service.
    struct Handle *slot = handle_get(p, service_h);
    if (!slot)                                          return -1;
    if (slot->kind != KOBJ_SRV)                         return -1;
    if ((slot->rights & RIGHT_READ) != RIGHT_READ)      return -1;
    if (!slot->obj)                                     return -1;
    if (*(const u64 *)slot->obj != SRV_SERVICE_MAGIC)   return -1;
    struct SrvService *svc = (struct SrvService *)slot->obj;

    // Poster gate: only the Proc currently posting this service may accept
    // its connections. Holding the service handle is already evidence; the
    // stripes match additionally rejects a stale handle into a service
    // that was tombstoned and rebound by a different poster.
    u64 caller = proc_stripes(p);
    if (caller == 0)                                    return -1;
    if (svc->poster_stripes != caller)                  return -1;

    // Block until a connection is on the backlog. NULL means the service
    // stopped being LIVE while we blocked (the poster exited / a test
    // reset the registry) — fail closed.
    struct SrvConn *cn = srv_accept_blocking(svc);
    if (!cn) return -1;

    // Wrap the accepted SrvConn in a devsrv connection Spoor — corvus's
    // server endpoint. The SrvConn reference held by the backlog passes
    // to the Spoor.
    struct Spoor *conn_spoor = devsrv_make_conn_spoor(cn);
    if (!conn_spoor) {
        // Could not build the endpoint: tear the connection down so the
        // client wakes with EOF rather than waiting on a server it will
        // never reach, then drop the (now sole) backlog reference.
        srvconn_teardown(cn);
        srvconn_unref(cn);
        return -1;
    }

    // Install the server endpoint as a KObj_Spoor handle. handle_alloc
    // takes ownership of conn_spoor's reference (from spoor_alloc inside
    // devsrv_make_conn_spoor); close runs spoor_clunk → devsrv_close →
    // srvconn_unref.
    hidx_t ch = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE,
                             conn_spoor);
    if (ch < 0) {
        spoor_clunk(conn_spoor);   // → devsrv_close → srvconn_unref
        return -1;
    }
    return (int)ch;
}

static s64 sys_srv_accept_handler(u64 service_h_raw) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;
    return (s64)sys_srv_accept_for_proc(p, (hidx_t)service_h_raw);
}

// =============================================================================
// SYS_SRV_PEER — read a /srv connection's kernel-stamped peer identity.
// =============================================================================
//
// corvus calls SYS_SRV_PEER per request to learn who is on the other end
// of a connection (CORVUS-DESIGN.md §6.3; invariant C-22). The peer
// identity is stamped by the kernel — never supplied by the client,
// never cached on corvus's fid state. Spec contract specs/corvus.tla:
// SrvPeerOp resolves the peer FRESH every op; ConnOpPeerWasLive — a dead
// peer fail-closes.
//
// Returns 0 on success (*out filled), -1 on failure.
int sys_srv_peer_for_proc(struct Proc *p, hidx_t conn_h,
                          struct srv_peer_info *out) {
    if (!p || !out) return -1;

    // Resolve the connection handle: a KObj_Spoor endpoint the caller
    // holds (SYS_SRV_ACCEPT minted it). RIGHT_READ is defense-in-depth —
    // the accept installs READ|WRITE on the endpoint.
    struct Spoor *sp = sys_lookup_spoor(p, conn_h, RIGHT_READ);
    if (!sp) return -1;

    // The Spoor must be a devsrv connection Spoor; devsrv_conn_of returns
    // NULL for a pipe / dev9p / devsrv-root / devsrv-service Spoor.
    struct SrvConn *cn = devsrv_conn_of(sp);
    if (!cn) return -1;

    // Poster gate (CORVUS-DESIGN §6.3): only the service's poster may
    // query a connection's peer. The SrvConn captured the poster's
    // stripes by value at mint; the caller's stripes must match.
    u64 caller = proc_stripes(p);
    if (caller == 0)                            return -1;
    if (srvconn_server_stripes(cn) != caller)   return -1;

    // Immutable identity — captured by value on the SrvConn at mint, so
    // it is available even after the peer exits (no Proc lookup, no UAF).
    u64  peer_stripes = srvconn_peer_stripes(cn);
    bool peer_console = srvconn_peer_console(cn);

    // Live caps + identity + the dead-Proc guard: re-find the peer by
    // stripes under the process-table lock. A peer that exited / is a
    // zombie / was reaped has no ALIVE Proc carrying its stripes — `caps`
    // + `alive` + identity all fail-close (never a stale snapshot).
    // A-1a: one walk snapshots caps + principal_id + primary_gid.
    caps_t peer_caps      = 0;
    u32    peer_principal = PRINCIPAL_NONE;
    u32    peer_gid       = GID_NONE;
    bool   peer_alive = proc_peer_snapshot_by_stripes(peer_stripes, &peer_caps,
                                                      &peer_principal, &peer_gid);

    out->stripes      = peer_stripes;
    out->caps         = peer_alive ? (u64)peer_caps : 0u;
    out->console      = peer_console ? 1u : 0u;
    out->alive        = peer_alive ? 1u : 0u;
    // A-1a: identity resolved fresh per query; a dead peer fail-closes to
    // NONE (the SrvConn captures only stripes + console immutably).
    out->principal_id = peer_alive ? peer_principal : PRINCIPAL_NONE;
    out->primary_gid  = peer_alive ? peer_gid       : GID_NONE;
    out->flags        = 0u;
    out->_reserved    = 0u;
    return 0;
}

static s64 sys_srv_peer_handler(u64 conn_h_raw, u64 out_va) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    // The result crosses to a user-VA buffer; validate the range before
    // the per-byte store (uaccess_store_u8 does not range-check).
    if (!sys_validate_user_buf(out_va, sizeof(struct srv_peer_info)))
        return -1;

    struct srv_peer_info info = {0};
    if (sys_srv_peer_for_proc(p, (hidx_t)conn_h_raw, &info) != 0)
        return -1;

    // Store the struct per-byte with fault fixup (the sys_wait_pid_handler
    // pattern). On a partial-write fault, scrub the bytes already written
    // so userspace can never read a torn peer identity, then fail.
    const u8 *bytes = (const u8 *)&info;
    for (u64 i = 0; i < sizeof(info); i++) {
        if (uaccess_store_u8(out_va + i, bytes[i]) != 0) {
            for (u64 j = 0; j < i; j++)
                (void)uaccess_store_u8(out_va + j, 0);
            return -1;
        }
    }
    return 0;
}

// =============================================================================
// SYS_SRV_CONNECT — client-open the /srv mechanism (P5-corvus-srv-impl-b2).
//
// Per CORVUS-DESIGN.md §6.2. Composes srv_conn_open_for_proc (which
// already mints + enqueues the SrvConn and installs a non-transferable
// KObj_Srv handle in the caller's table — landed at -a3b) with the new
// srvconn_drive_client_handshake (Tversion + Tattach + optional Twalk +
// Tlopen on the SrvConn's kernel-owned p9_client). On success, the
// returned KObj_Srv handle has an OPEN client_fid; SYS_READ / SYS_WRITE
// on the handle translate to Tread / Twrite at that fid.
//
// Per-Proc cap: srv_conn_open_for_proc rejects when p->srv_conn_count
// is already at SRV_CONN_PER_PROC_MAX (1 at v1.0). A subsequent
// SYS_SRV_CONNECT from the same Proc must wait until the existing
// handle is SYS_CLOSE'd (which decrements the count via handle_close's
// KOBJ_SRV SRV_CONN_MAGIC arm).
//
// On any handshake failure (server crashed / hung / Rlerror / OOM), the
// handler closes the just-installed handle — handle_close's release
// arm tears the SrvConn down + drops both the create reference and the
// backlog-side reference (via srv_proc_exit_notify on the SERVER side
// when the backlog is drained, or directly when the accept thread
// pops it and finds it torn).
// =============================================================================

int sys_srv_connect_for_proc(struct Proc *p,
                              const char *name, size_t name_len,
                              const u8 *path, size_t path_len) {
    if (!p)                                              return -1;
    if (!name || name_len == 0 || name_len > SRV_NAME_MAX) return -1;
    if (path_len > SRVCONN_PATH_MAX)                     return -1;
    if (path_len > 0 && !path)                           return -1;

    // Phase 1: mint + install the KObj_Srv handle + bump p->srv_conn_count.
    // The cap check + per-Proc bump happen inside srv_conn_open_for_proc;
    // a failure here leaves no state behind. The mint also propagates
    // the service's transport mode onto the SrvConn (cn->byte_mode is
    // set BEFORE Phase 1 enqueues the SrvConn on the accept backlog;
    // we read it here without a lock).
    int h = srv_conn_open_for_proc(p, name, (u8)name_len);
    if (h < 0) return -1;

    // Phase 2: drive the kernel-side 9P handshake against the now-installed
    // SrvConn. Look the handle up to reach the SrvConn — srv_conn_open_for_
    // proc just installed it under the caller's lock, so the handle is
    // observable here. handle_get validates HANDLE_MAGIC + bounds.
    struct Handle *slot = handle_get(p, (hidx_t)h);
    if (!slot || slot->kind != KOBJ_SRV || !slot->obj) {
        // Should not happen — srv_conn_open_for_proc just installed it.
        handle_close(p, (hidx_t)h);
        return -1;
    }
    if (*(const u64 *)slot->obj != SRV_CONN_MAGIC) {
        handle_close(p, (hidx_t)h);
        return -1;
    }
    struct SrvConn *cn = (struct SrvConn *)slot->obj;

    // P6-pouch-sockets (sub-chunk 12): byte-mode services have no
    // handshake — the SrvConn is ready for raw byte I/O the moment it
    // is enqueued. A non-empty path makes no sense in byte mode (there
    // is no 9P fid to walk against); reject up-front.
    // F5 close (P6-pouch-sockets audit): atomic acquire pairs the
    // srvconn_set_byte_mode release in srv_conn_open_for_proc.
    if (__atomic_load_n(&cn->byte_mode, __ATOMIC_ACQUIRE)) {
        if (path_len > 0) {
            handle_close(p, (hidx_t)h);
            return -1;
        }
        return h;
    }

    // F1 close (P5-corvus-srv-impl audit): bound the handshake on the
    // wall clock. Without this every Tversion/Tattach/Twalk/Tlopen
    // recv falls through to a deadline-0 tsleep — "no deadline" —
    // wedging this Proc indefinitely on a corvus that posted but is
    // crashed / spinning / not draining. With the deadline a hung
    // corvus returns -1 here, the handle teardown fires, and the
    // caller can retry or surface the error to userspace.
    srvconn_set_client_deadline(cn, timer_now_ns() + SRVCONN_HANDSHAKE_DEADLINE_NS);
    if (srvconn_drive_client_handshake(cn, p->pid, path, path_len) != 0) {
        // Handshake failed — corvus crashed / hung / refused / OOM /
        // deadline-elapsed. Tear the just-installed handle down.
        // handle_close's KOBJ_SRV path teardowns + unrefs the SrvConn
        // AND decrements srv_conn_count.
        handle_close(p, (hidx_t)h);
        return -1;
    }

    return h;
}

static s64 sys_srv_connect_handler(u64 name_va, u64 name_len_raw,
                                    u64 path_va, u64 path_len_raw) {
    struct Thread *t = current_thread();
    if (!t)                                              return -1;
    struct Proc *p = t->proc;
    if (!p)                                              return -1;

    if (name_len_raw == 0 || name_len_raw > SRV_NAME_MAX) return -1;
    if (path_len_raw > SRVCONN_PATH_MAX)                  return -1;

    // F6 close (P5-corvus-srv-impl audit): pre-validate the full user-
    // VA range for each buffer BEFORE the per-byte copy loops. Matches
    // the discipline of every other handler that takes a user buffer
    // (sys_spawn_*, sys_srv_peer). The per-byte uaccess_load_u8 IS
    // fault-safe, but pre-validation catches a bad address before
    // any byte lands in kernel scratch.
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -1;
    if (path_len_raw > 0 && !sys_validate_user_buf(path_va, path_len_raw))
        return -1;

    // Service name copy-in (per-byte uaccess_load_u8; same shape as
    // SYS_POST_SERVICE).
    char name[SRV_NAME_MAX];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(name_va + i, &b) != 0)        return -1;
        name[i] = (char)b;
    }

    // Path copy-in. path_len_raw == 0 is the "no Twalk" case
    // (Tversion + Tattach only, open at root_fid).
    u8 path[SRVCONN_PATH_MAX];
    for (u64 i = 0; i < path_len_raw; i++) {
        if (uaccess_load_u8(path_va + i, &path[i]) != 0)  return -1;
    }

    return (s64)sys_srv_connect_for_proc(p, name, (size_t)name_len_raw,
                                          path, (size_t)path_len_raw);
}

// =============================================================================
// SYS_CAP_GRANT / SYS_CAP_USE — userspace bridges to the `cap` device
// (P5-hostowner-b-b; CORVUS-DESIGN.md §5.5.1).
//
// The cap device exposes /cap/grant + /cap/use through the Dev write op
// (devcap_write), the eventual production path through a future
// namespace-aware open syscall. At v1.0 there is no t_open in userspace,
// so the two writers (corvus → grant; the console-attached redeemer →
// use) reach the cores directly via these syscalls. Same gate semantics
// as the Dev op — the cores (cap_register_grant_for_writer /
// cap_redeem_grant_for_writer) enforce both.
//
// Both syscalls return 0 on success (a synthetic "wrote frame" ack; we
// don't echo the byte count since this is a syscall, not an fd write)
// and -1 on any gate fail / bad args / table full / no pending grant.
// =============================================================================

static s64 sys_cap_grant_handler(u64 cap_mask, u64 target_stripes) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                                    return -1;
    long rc = cap_register_grant_for_writer(t->proc, (caps_t)cap_mask,
                                             target_stripes);
    return (rc >= 0) ? 0 : -1;
}

static s64 sys_cap_use_handler(u64 cap_mask) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                                    return -1;
    long rc = cap_redeem_grant_for_writer(t->proc, (caps_t)cap_mask);
    return (rc >= 0) ? 0 : -1;
}

// SYS_CAP_GRANT_CLEARANCE — the A-4a clearance grant-side bridge (corvus is
// chrooted; it reaches the cap device by syscall, like the hostowner grant).
// Forwards to cap_register_clearance_grant_for_writer, which enforces the
// CAP_GRANT_CLEARANCE gate + all bounds. The redeem rides SYS_CAP_USE.
static s64 sys_cap_grant_clearance_handler(u64 cap_mask, u64 target_stripes,
                                           u64 valid_for_ns, u64 session_id) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                                    return -1;
    long rc = cap_register_clearance_grant_for_writer(
        t->proc, (caps_t)cap_mask, target_stripes, valid_for_ns, session_id);
    return (rc >= 0) ? 0 : -1;
}

// =============================================================================
// SYS_POLL — the multi-fd wait/wake primitive (P5-poll-a).
//
// Per ARCH §23.3 + specs/poll.tla + <thylacine/poll.h>. The user
// passes a `struct pollfd[nfds]` array via user-VA; the handler copies
// the array in (so `fd` + `events` are read once before any sleep),
// hands it to `sys_poll_for_proc`, and writes back the `revents`
// field of each pollfd. The unchanged `fd`/`events` bytes are not
// rewritten — only `revents` (the kernel's output) crosses back to
// user-VA. On a partial-write fault, already-written revents bytes
// are scrubbed to 0 so userspace can never observe a torn revents
// state.
// =============================================================================

static s64 sys_poll_handler(u64 fds_va, u64 nfds_raw, u64 timeout_ms_raw) {
    struct Thread *t = current_thread();
    if (!t)                                                  return -1;
    struct Proc *p = t->proc;
    if (!p)                                                  return -1;

    // nfds bound — same as the testable core's check, but also the
    // stack-array bound on `kfds[]` below. Reject before touching
    // user-VA.
    if (nfds_raw == 0 || nfds_raw > PROC_HANDLE_MAX)         return -1;
    u64 nfds = nfds_raw;

    // User-VA range check on the entire pollfd[] array.
    u64 buf_bytes = nfds * sizeof(struct pollfd);
    if (!sys_validate_user_buf(fds_va, buf_bytes))           return -1;

    // Copy in: read all 8 bytes per pollfd. The kfds[] array is the
    // canonical fd+events the kernel operates on; this snapshot is
    // taken once at entry so the values can't change mid-sleep under
    // a concurrent userspace mutation.
    struct pollfd kfds[PROC_HANDLE_MAX];
    u8 *kbytes = (u8 *)kfds;
    for (u64 i = 0; i < buf_bytes; i++) {
        if (uaccess_load_u8(fds_va + i, &kbytes[i]) != 0)    return -1;
    }

    // s32 cast for timeout — Linux semantics: negative = block forever,
    // 0 = non-blocking, positive = ms. The raw u64 we get from x2
    // truncates to s32 here.
    s32 timeout_ms = (s32)(s64)timeout_ms_raw;

    s64 result = sys_poll_for_proc(p, kfds, nfds, timeout_ms);
    if (result < 0) return result;

    // Write back: only the `revents` field (2 bytes at offset 6 per
    // pollfd). Per-byte uaccess_store_u8 with fault fixup; on a partial
    // fault, scrub the bytes already written back to zero so userspace
    // can't observe a torn revents (sys_srv_peer_handler pattern).
    for (u64 i = 0; i < nfds; i++) {
        u64 rev_va  = fds_va + i * sizeof(struct pollfd)
                              + __builtin_offsetof(struct pollfd, revents);
        const u8 *rb = (const u8 *)&kfds[i].revents;
        for (u64 j = 0; j < sizeof(kfds[i].revents); j++) {
            if (uaccess_store_u8(rev_va + j, rb[j]) != 0) {
                // Scrub everything we've written so far — this pollfd
                // plus every earlier one.
                for (u64 ii = 0; ii <= i; ii++) {
                    u64 sva = fds_va + ii * sizeof(struct pollfd)
                                       + __builtin_offsetof(struct pollfd, revents);
                    u64 lim = (ii == i) ? j : sizeof(kfds[ii].revents);
                    for (u64 jj = 0; jj < lim; jj++) {
                        (void)uaccess_store_u8(sva + jj, 0);
                    }
                }
                return -1;
            }
        }
    }
    return result;
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

    case SYS_EXIT_GROUP:
        // Never returns. POSIX exit_group(2): terminate the WHOLE Proc
        // (cascade peer Threads via proc_group_terminate), not just the
        // calling Thread. The exception_context stays on the EXITING thread's
        // kstack until wait_pid -> thread_free.
        sys_exit_group_handler(ctx->regs[0]);

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

    case SYS_MMIO_MAP:
        ctx->regs[0] = (u64)sys_mmio_map_handler(ctx->regs[0],
                                                 ctx->regs[1],
                                                 ctx->regs[2]);
        return;

    case SYS_DMA_CREATE:
        ctx->regs[0] = (u64)sys_dma_create_handler(ctx->regs[0],
                                                   ctx->regs[1]);
        return;

    case SYS_DMA_MAP:
        ctx->regs[0] = (u64)sys_dma_map_handler(ctx->regs[0],
                                                ctx->regs[1],
                                                ctx->regs[2]);
        return;

    case SYS_PIPE: {
        // sys_pipe_handler writes the read-end fd to *out_rd and the
        // write-end fd to *out_wr on success. On error, returns -1 and
        // both Spoors are clunked; ctx->regs[1] is unmodified.
        u64 rd_fd = 0, wr_fd = 0;
        s64 rc = sys_pipe_handler(&rd_fd, &wr_fd);
        if (rc < 0) {
            ctx->regs[0] = (u64)(s64)-1;
        } else {
            ctx->regs[0] = rd_fd;
            ctx->regs[1] = wr_fd;
        }
        return;
    }

    case SYS_READ:
        ctx->regs[0] = (u64)sys_read_handler(ctx->regs[0],
                                             ctx->regs[1],
                                             ctx->regs[2]);
        return;

    case SYS_WRITE:
        ctx->regs[0] = (u64)sys_write_handler(ctx->regs[0],
                                              ctx->regs[1],
                                              ctx->regs[2]);
        return;

    case SYS_CLOSE:
        ctx->regs[0] = (u64)sys_close_handler(ctx->regs[0]);
        return;

    case SYS_DUP:
        ctx->regs[0] = (u64)sys_dup_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_ATTACH_9P:
        ctx->regs[0] = (u64)sys_attach_9p_handler(ctx->regs[0],
                                                  ctx->regs[1],
                                                  ctx->regs[2],
                                                  ctx->regs[3],
                                                  ctx->regs[4]);
        return;

    case SYS_MOUNT:
        ctx->regs[0] = (u64)sys_mount_handler(ctx->regs[0],
                                              ctx->regs[1],
                                              ctx->regs[2]);
        return;

    case SYS_UNMOUNT:
        ctx->regs[0] = (u64)sys_unmount_handler(ctx->regs[0]);
        return;

    case SYS_MLOCKALL:
        ctx->regs[0] = (u64)sys_mlockall_handler(ctx->regs[0]);
        return;

    case SYS_SET_DUMPABLE:
        ctx->regs[0] = (u64)sys_set_dumpable_handler(ctx->regs[0]);
        return;

    case SYS_SET_TRACEABLE:
        ctx->regs[0] = (u64)sys_set_traceable_handler(ctx->regs[0]);
        return;

    case SYS_EXPLICIT_BZERO:
        ctx->regs[0] = (u64)sys_explicit_bzero_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_GETRANDOM:
        ctx->regs[0] = (u64)sys_getrandom_handler(ctx->regs[0],
                                                  ctx->regs[1],
                                                  ctx->regs[2]);
        return;

    case SYS_SPAWN:
        ctx->regs[0] = (u64)sys_spawn_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_WAIT_PID:
        ctx->regs[0] = (u64)sys_wait_pid_handler(ctx->regs[0]);
        return;

    case SYS_SPAWN_WITH_FDS:
        ctx->regs[0] = (u64)sys_spawn_with_fds_handler(ctx->regs[0],
                                                       ctx->regs[1],
                                                       ctx->regs[2],
                                                       ctx->regs[3]);
        return;

    case SYS_SPAWN_WITH_CAPS:
        ctx->regs[0] = (u64)sys_spawn_with_caps_handler(ctx->regs[0],
                                                        ctx->regs[1],
                                                        ctx->regs[2]);
        return;

    case SYS_SPAWN_FULL:
        ctx->regs[0] = (u64)sys_spawn_full_handler(ctx->regs[0],
                                                   ctx->regs[1],
                                                   ctx->regs[2],
                                                   ctx->regs[3],
                                                   ctx->regs[4]);
        return;

    case SYS_POST_SERVICE:
        ctx->regs[0] = (u64)sys_post_service_handler(ctx->regs[0],
                                                     ctx->regs[1]);
        return;

    case SYS_POST_SERVICE_BYTE:
        ctx->regs[0] = (u64)sys_post_service_byte_handler(ctx->regs[0],
                                                           ctx->regs[1]);
        return;

    case SYS_SRV_ACCEPT:
        ctx->regs[0] = (u64)sys_srv_accept_handler(ctx->regs[0]);
        return;

    case SYS_SRV_PEER:
        ctx->regs[0] = (u64)sys_srv_peer_handler(ctx->regs[0],
                                                 ctx->regs[1]);
        return;

    case SYS_POLL:
        ctx->regs[0] = (u64)sys_poll_handler(ctx->regs[0],
                                             ctx->regs[1],
                                             ctx->regs[2]);
        return;

    case SYS_SRV_CONNECT:
        ctx->regs[0] = (u64)sys_srv_connect_handler(ctx->regs[0],
                                                     ctx->regs[1],
                                                     ctx->regs[2],
                                                     ctx->regs[3]);
        return;

    case SYS_SPAWN_WITH_PERMS:
        ctx->regs[0] = (u64)sys_spawn_with_perms_handler(ctx->regs[0],
                                                          ctx->regs[1],
                                                          ctx->regs[2],
                                                          ctx->regs[3],
                                                          ctx->regs[4],
                                                          ctx->regs[5]);
        return;

    case SYS_SPAWN_FULL_ARGV:
        ctx->regs[0] = (u64)sys_spawn_full_argv_handler(ctx->regs[0]);
        return;

    case SYS_FSTAT:
        ctx->regs[0] = (u64)sys_fstat_handler(ctx->regs[0],
                                              ctx->regs[1]);
        return;

    case SYS_LSEEK:
        ctx->regs[0] = (u64)sys_lseek_handler(ctx->regs[0],
                                              ctx->regs[1],
                                              ctx->regs[2]);
        return;

    case SYS_ATTACH_9P_SRV:
        ctx->regs[0] = (u64)sys_attach_9p_srv_handler(ctx->regs[0],
                                                       ctx->regs[1],
                                                       ctx->regs[2],
                                                       ctx->regs[3]);
        return;

    case SYS_PIVOT_ROOT:
        ctx->regs[0] = (u64)sys_pivot_root_handler(ctx->regs[0]);
        return;

    case SYS_CAP_GRANT:
        ctx->regs[0] = (u64)sys_cap_grant_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_CAP_USE:
        ctx->regs[0] = (u64)sys_cap_use_handler(ctx->regs[0]);
        return;

    case SYS_CAP_GRANT_CLEARANCE:
        ctx->regs[0] = (u64)sys_cap_grant_clearance_handler(
            ctx->regs[0], ctx->regs[1], ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_WALK_OPEN:
        ctx->regs[0] = (u64)sys_walk_open_handler(ctx->regs[0],
                                                  ctx->regs[1],
                                                  ctx->regs[2],
                                                  ctx->regs[3]);
        return;

    case SYS_WALK_CREATE:
        ctx->regs[0] = (u64)sys_walk_create_handler(ctx->regs[0],
                                                    ctx->regs[1],
                                                    ctx->regs[2],
                                                    ctx->regs[3],
                                                    ctx->regs[4]);
        return;

    case SYS_FSYNC:
        ctx->regs[0] = (u64)sys_fsync_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_READDIR:
        ctx->regs[0] = (u64)sys_readdir_handler(ctx->regs[0],
                                                ctx->regs[1],
                                                ctx->regs[2]);
        return;

    case SYS_RENAME:
        ctx->regs[0] = (u64)sys_rename_handler(ctx->regs[0],
                                               ctx->regs[1],
                                               ctx->regs[2],
                                               ctx->regs[3],
                                               ctx->regs[4],
                                               ctx->regs[5]);
        return;

    case SYS_UNLINK:
        ctx->regs[0] = (u64)sys_unlink_handler(ctx->regs[0],
                                               ctx->regs[1],
                                               ctx->regs[2],
                                               ctx->regs[3]);
        return;

    case SYS_WSTAT:
        ctx->regs[0] = (u64)sys_wstat_handler(ctx->regs[0],
                                              ctx->regs[1],
                                              ctx->regs[2],
                                              ctx->regs[3],
                                              ctx->regs[4]);
        return;

    case SYS_CHROOT:
        ctx->regs[0] = (u64)sys_chroot_handler(ctx->regs[0]);
        return;

    case SYS_SET_TID_ADDRESS:
        ctx->regs[0] = (u64)sys_set_tid_address_handler(ctx->regs[0]);
        return;

    case SYS_BURROW_ATTACH:
        ctx->regs[0] = (u64)sys_burrow_attach_handler(ctx->regs[0]);
        return;

    case SYS_BURROW_DETACH:
        ctx->regs[0] = (u64)sys_burrow_detach_handler(ctx->regs[0],
                                                      ctx->regs[1]);
        return;

    case SYS_TORPOR_WAIT:
        ctx->regs[0] = (u64)sys_torpor_wait_handler(ctx->regs[0],
                                                    ctx->regs[1],
                                                    ctx->regs[2]);
        return;

    case SYS_TORPOR_WAKE:
        ctx->regs[0] = (u64)sys_torpor_wake_handler(ctx->regs[0],
                                                    ctx->regs[1]);
        return;

    case SYS_THREAD_SPAWN:
        ctx->regs[0] = (u64)sys_thread_spawn_handler(ctx->regs[0],
                                                     ctx->regs[1],
                                                     ctx->regs[2],
                                                     ctx->regs[3]);
        return;

    case SYS_THREAD_EXIT:
        // Never returns. Kernel thread_exit_self() runs the
        // clear_child_tid handoff, marks self EXITING, and yields. The
        // exception_context stays on the EXITING thread's kstack until
        // the parent's wait_pid → thread_free.
        sys_thread_exit_handler();

    // P6-pouch-signals-impl (sub-chunk 13a): the 5 note syscalls.
    case SYS_NOTE_OPEN:
        ctx->regs[0] = (u64)sys_note_open_handler();
        return;

    case SYS_NOTIFY:
        ctx->regs[0] = (u64)sys_notify_handler(ctx->regs[0]);
        return;

    case SYS_NOTED:
        // sys_noted_handler manages ctx directly:
        //   - NCONT (arg=0): rewrites ctx with the saved pre-handler
        //     state; regs[0] becomes the saved value. NO post-write.
        //   - NDFLT (arg=1): exits, never returns.
        //   - Invalid: sets ctx->regs[0] = -1 internally.
        sys_noted_handler(ctx, ctx->regs[0]);
        return;

    case SYS_POSTNOTE:
        ctx->regs[0] = (u64)sys_postnote_handler(ctx->regs[0],
                                                  ctx->regs[1],
                                                  ctx->regs[2]);
        return;

    case SYS_NOTE_MASK:
        ctx->regs[0] = (u64)sys_note_mask_handler(ctx->regs[0],
                                                    ctx->regs[1]);
        return;

    default:
        // Unknown syscall. Phase 5+ delivers SIGSYS-equivalent note;
        // v1.0 returns -1 (ENOSYS) and lets userspace decide.
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
}
