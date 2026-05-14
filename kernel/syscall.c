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
#include <thylacine/dma_handle.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/spoor.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../arch/arm64/exception.h"
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
s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf, u64 len) {
    if (!p || (!kbuf && len > 0))                    return -1;
    struct Spoor *c = sys_lookup_spoor(p, h, RIGHT_WRITE);
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->write)                   return -1;
    if (len == 0)                                    return 0;

    long n = c->dev->write(c, kbuf, (long)len, c->offset);
    if (n < 0)                                       return -1;
    c->offset += n;
    return (s64)n;
}

// Inner — testable with kernel-side buf. Returns bytes read (>=0; 0
// on EOF) or -1 on bad handle / wrong kind / missing rights / dev error.
s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len) {
    if (!p || (!kbuf && len > 0))                    return -1;
    struct Spoor *c = sys_lookup_spoor(p, h, RIGHT_READ);
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->read)                    return -1;
    if (len == 0)                                    return 0;

    long n = c->dev->read(c, kbuf, (long)len, c->offset);
    if (n < 0)                                       return -1;
    c->offset += n;
    return (s64)n;
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
        // len). Returns 0 if the handle is good.
        struct Spoor *c = sys_lookup_spoor(p, (hidx_t)hraw, RIGHT_WRITE);
        if (!c)                                      return -1;
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
        struct Spoor *c = sys_lookup_spoor(p, (hidx_t)hraw, RIGHT_READ);
        if (!c)                                      return -1;
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

    // Allocate the adapter on the heap so its lifetime is tied to
    // the attached (dev9p_close frees it via priv->adapter_to_free).
    struct p9_spoor_transport *adapter = kmalloc(sizeof(*adapter), KP_ZERO);
    if (!adapter) {
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        return -1;
    }
    // owns_spoors=false: the SVC handler manages the Spoor refs
    // explicitly. dev9p_close's attach-teardown branch (which fires
    // after p9_attached_destroy / p9_client_close → adapter close →
    // no-op since owns=false) does the spoor_unref's.
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
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        kfree(adapter);
        return -1;
    }

    struct Spoor *root = p9_attached_root_spoor(att);
    if (!root) {
        p9_attached_destroy(att);
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        kfree(adapter);
        return -1;
    }

    // Patch the root Spoor's dev9p_priv with the attach-session
    // ownership pointers. dev9p_close on this Spoor will then run the
    // full teardown sequence (destroy attached → unref transport
    // Spoors → kfree adapter).
    struct dev9p_priv *root_priv = (struct dev9p_priv *)root->aux;
    if (!root_priv || root_priv->magic != DEV9P_PRIV_MAGIC) {
        // Shouldn't happen — p9_attached_root_spoor's dev9p Spoor
        // always has a valid priv. Defensive rollback.
        spoor_clunk(root);
        p9_attached_destroy(att);
        spoor_unref(tx);
        if (rx != tx) spoor_unref(rx);
        kfree(adapter);
        return -1;
    }
    root_priv->attached_owner  = att;
    root_priv->adapter_to_free = adapter;

    // Install root Spoor as a KOBJ_SPOOR handle. handle_alloc takes
    // ownership of root's ref (the one from spoor_alloc inside
    // p9_attached_root_spoor).
    rights_t r = RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, r, root);
    if (fd < 0) {
        // Roll back: clunking root triggers dev9p_close, which sees
        // attached_owner non-NULL and tears down the entire session.
        spoor_clunk(root);
        return -1;
    }
    return (s64)fd;
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

    default:
        // Unknown syscall. Phase 5+ delivers SIGSYS-equivalent note;
        // v1.0 returns -1 (ENOSYS) and lets userspace decide.
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
}
