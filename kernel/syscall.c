// Userspace syscall dispatch implementation (P3-Ec).
//
// At v1.0 P3-Ec the syscall surface is intentionally tiny — exits +
// puts. Just enough for an EL0 thread to signal "I ran, and here's the
// result" to the kernel test harness. Phase 5+ adds the full syscall
// surface; each syscall lands in its own TU.

#include <thylacine/syscall.h>
#include <thylacine/9p_attach.h>
#include <thylacine/9p_spoor_transport.h>
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
#include <thylacine/page.h>
#include <thylacine/pipe.h>
#include <thylacine/poll.h>
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
    int rc = burrow_map(p, b, vaddr, km->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        return -1;
    }
    burrow_unref(b);
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
    int rc = burrow_map(p, b, vaddr, kd->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        return -1;
    }
    burrow_unref(b);

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
        struct SrvConn *cn = (struct SrvConn *)slot->obj;
        srvconn_set_client_deadline(cn, timer_now_ns() + SRVCONN_OP_DEADLINE_NS);
        long n = srvconn_client_write(cn, kbuf, (long)len);
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
        struct SrvConn *cn = (struct SrvConn *)slot->obj;
        srvconn_set_client_deadline(cn, timer_now_ns() + SRVCONN_OP_DEADLINE_NS);
        long n = srvconn_client_read(cn, kbuf, (long)len);
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
    struct p9_attached *att = p9_attached_create(
        ops,
        SYS_ATTACH_DEFAULT_MSIZE,            // recv_cap (= msize at v1.0)
        SYS_ATTACH_DEFAULT_ROOT_FID,         // root_fid
        SYS_ATTACH_DEFAULT_MSIZE,            // msize (client proposal)
        NULL, 0,                             // uname (empty at v1.0; no-auth)
        aname_len > 0 ? aname_scratch : NULL, aname_len,
        (u32)n_uname);
    if (!att) {
        // p9_attached_create's failure leaves the adapter untouched
        // (the create's transport_ops.close runs on rollback, which is
        // a no-op for owns=false). We must still kfree the adapter +
        // release transport refs since they never transferred.
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        kfree(adapter);
        return -1;
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
    // dev9p's walkqid carrier has nc as its spoor; we own + free it
    // now that we've consumed the walk result (we don't need the qids
    // returned in w->qid[] — the next open() will refresh nc->qid).
    walkqid_free(w);

    // Issue the open. dev9p_open returns nc itself (state mutated:
    // COPEN flag set, mode + offset reset, qid refreshed from Rlopen).
    // On failure nc still has its own walk-allocated fid; spoor_clunk
    // runs dev->close → p9_client_clunk on that fid + frees the priv.
    if (!nc->dev->open(nc, (int)omode_raw)) {
        spoor_clunk(nc);
        return -1;
    }

    // Install the now-opened nc in the caller's handle table. The
    // rights envelope matches SYS_ATTACH_9P: R | W | TRANSFER. The
    // server enforces the actual omode (writes to an OREAD-only fid
    // return Rlerror, not -EPERM here). handle_alloc takes ownership
    // of the ref from spoor_clone; on full-table failure we spoor_clunk
    // (running dev->close to clunk the fid).
    //
    // F7 close (P5-stratumd-stub-bringup audit): the R|W|TRANSFER mask
    // delegates write/transfer gating to the underlying Dev. For dev9p
    // — the only user-reachable walk source at v1.0 — the 9P server is
    // the authority; the kernel rights envelope is a hint, not the
    // enforcement gate. A future Dev whose walk requires kernel rights
    // for enforcement MUST derive returned rights from the source slot
    // (e.g., slot->rights), not from this hardcoded mask, OR sit behind
    // a per-Dev policy hook that intersects this mask with Dev-defined
    // upper bounds. At v1.0 the policy is "9P-server-mediated"; this
    // comment is the contract for future walkable Devs.
    rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, r, nc);
    if (fd < 0) {
        spoor_clunk(nc);
        return -1;
    }
    return (s64)fd;
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
    p->proc_flags |= PROC_FLAG_MLOCKED;
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
        p->proc_flags |= PROC_FLAG_NODUMP;
        return 0;
    }
    if (dumpable == 1) {
        // Refuse re-enable: corvus's no-coredump posture is one-way.
        if (p->proc_flags & PROC_FLAG_NODUMP)        return -1;
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
        p->proc_flags |= PROC_FLAG_NOTRACE;
        return 0;
    }
    if (traceable == 1) {
        if (p->proc_flags & PROC_FLAG_NOTRACE)       return -1;
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
// SYS_POST_SERVICE — register the caller as a /srv service (P5-corvus-srv).
// =============================================================================
//
// Registers the calling Proc as the 9P server for /srv/<name> and returns
// a KObj_Srv service handle. CORVUS-DESIGN.md §6.1; spec contract
// specs/corvus.tla::PostService (the marked-poster gate + the reserve/
// commit two-phase).
//
// Returns the service handle (hidx ≥ 0) on success, -1 on failure.
int sys_post_service_for_proc(struct Proc *p, const char *name,
                              size_t name_len) {
    if (!p)                                            return -1;
    if (!name)                                         return -1;
    if (name_len == 0 || name_len > SRV_NAME_MAX)       return -1;

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
    if (srv_reserve(name, (u8)name_len, p, &svc, &prior) != 0) return -1;

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

static s64 sys_post_service_handler(u64 name_va, u64 name_len_raw) {
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

    return (s64)sys_post_service_for_proc(p, name, (size_t)name_len_raw);
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

    // Live caps + the dead-Proc guard: re-find the peer by stripes under
    // the process-table lock. A peer that exited / is a zombie / was
    // reaped has no ALIVE Proc carrying its stripes — `caps` and `alive`
    // both fail-close to 0 (never a stale snapshot).
    caps_t peer_caps  = 0;
    bool   peer_alive = proc_caps_by_stripes(peer_stripes, &peer_caps);

    out->stripes = peer_stripes;
    out->caps    = peer_alive ? (u64)peer_caps : 0u;
    out->console = peer_console ? 1u : 0u;
    out->alive   = peer_alive ? 1u : 0u;
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
    // a failure here leaves no state behind.
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

    case SYS_CAP_GRANT:
        ctx->regs[0] = (u64)sys_cap_grant_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_CAP_USE:
        ctx->regs[0] = (u64)sys_cap_use_handler(ctx->regs[0]);
        return;

    case SYS_WALK_OPEN:
        ctx->regs[0] = (u64)sys_walk_open_handler(ctx->regs[0],
                                                  ctx->regs[1],
                                                  ctx->regs[2],
                                                  ctx->regs[3]);
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

    default:
        // Unknown syscall. Phase 5+ delivers SIGSYS-equivalent note;
        // v1.0 returns -1 (ENOSYS) and lets userspace decide.
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
}
