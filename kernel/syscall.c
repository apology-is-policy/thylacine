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
#include <thylacine/devsrv.h>
#include <thylacine/dma_handle.h>
#include <thylacine/elf.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/pipe.h>
#include <thylacine/proc.h>
#include <thylacine/random.h>
#include <thylacine/spoor.h>
#include <thylacine/srvconn.h>
#include <thylacine/territory.h>
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
};

__attribute__((noreturn))
static void sys_spawn_with_fds_thunk(void *arg) {
    struct spawn_with_fds_args *sa = (struct spawn_with_fds_args *)arg;
    void   *blob       = sa->blob;
    size_t  blob_size  = sa->blob_size;
    u32     fd_count   = sa->fd_count;
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

int sys_spawn_full_for_proc(struct Proc *p, const char *name, size_t name_len,
                            const u32 *fds, u32 fd_count, caps_t cap_mask) {
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

    int pid = rfork_with_caps(RFPROC, sys_spawn_with_fds_thunk, sa, cap_mask);
    if (pid < 0) {
        kfree(sa);
        kfree(blob_copy);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    return pid;
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

    default:
        // Unknown syscall. Phase 5+ delivers SIGSYS-equivalent note;
        // v1.0 returns -1 (ENOSYS) and lets userspace decide.
        ctx->regs[0] = (u64)(s64)-1;
        return;
    }
}
