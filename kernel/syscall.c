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
#include <thylacine/errno.h>
#include <thylacine/allowance.h>
#include <thylacine/exec.h>
#include <thylacine/extinction.h>
#include <thylacine/handle.h>
#include <thylacine/irqfwd.h>
#include <thylacine/loom.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/notes.h>
#include <thylacine/page.h>
#include <thylacine/path.h>    // struct Path (SYS_FD2PATH reads ->s/->len; #66)
#include <thylacine/pci_handle.h>   // KObj_PCI (SYS_PCI_CLAIM/MAP_BAR/INFO; pci-1c)
#include <thylacine/pipe.h>
#include <thylacine/poll.h>
#include <thylacine/perm.h>
#include <thylacine/joey.h>     // boot_mark_complete (SYS_BOOT_COMPLETE)
#include <thylacine/proc.h>
#include <thylacine/random.h>
#include <thylacine/sched.h>
#include <thylacine/spinlock.h>
#include <thylacine/spoor.h>
#include <thylacine/stalk.h>
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
    // R9 F146 / RW-5 R3-F4: atomic load of p->caps. There IS a cross-thread
    // writer now -- proc_become_legate (the A-4a clearance redeem) does an
    // __atomic_fetch_or on a sibling thread's caps -- so the ACQUIRE load is
    // load-bearing, not just future-proofing. ldar is one instruction on aarch64.
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;

    // Validate rights early so a buggy caller doesn't allocate a KObj
    // we'll have to immediately free.
    if (rights == 0 || (rights & ~(u64)RIGHT_ALL))   return -1;

    // I-34 CreateBegin (specs/allowance.tla): if the caller carries a NARROWED
    // hardware allowance, [pa, pa+size) must lie within it. A broad (NULL)
    // allowance -- the warden + the existing trusted servers -- passes here;
    // the kobj_mmio_create I-5 reservation below still bounds it.
    if (!allowance_permits(p, HW_RES_MMIO, pa, size))  return -1;

    // P4-Ib HwResourceExclusive enforced by kobj_mmio_create: returns
    // NULL on overlap, bad alignment, size 0, OOM, or table-full.
    struct KObj_MMIO *k = kobj_mmio_create(pa, (size_t)size);
    if (!k)                                          return -1;

    // I-34 CreateCommit: install under the allowance re-check, so a concurrent
    // proc_revoke_allowance (DeviceRemoved) aborts this create instead of
    // leaking a handle through a being-revoked allowance.
    hidx_t h = allowance_handle_alloc(p, KOBJ_MMIO, (rights_t)rights, k);
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

    // R9 F146 / RW-5 R3-F4: atomic load of p->caps. There IS a cross-thread
    // writer now -- proc_become_legate (the A-4a clearance redeem) does an
    // __atomic_fetch_or on a sibling thread's caps -- so the ACQUIRE load is
    // load-bearing, not just future-proofing. ldar is one instruction on aarch64.
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

    // I-34 CreateBegin (specs/allowance.tla): a NARROWED allowance must list
    // this INTID. A broad (NULL) allowance -- the warden + the trusted servers
    // -- passes; the kobj_irq_create reservation below still bounds it.
    if (!allowance_permits(p, HW_RES_IRQ, intid, 0))   return -1;

    // INTID exclusivity enforced by kobj_irq_create's intid_try_claim
    // (P4-Ib addition); returns NULL on already-claimed / OOM /
    // gic_attach failure.
    struct KObj_IRQ *k = kobj_irq_create((u32)intid);
    if (!k)                                          return -1;

    // I-34 CreateCommit: install under the allowance re-check (revoke race).
    hidx_t h = allowance_handle_alloc(p, KOBJ_IRQ, (rights_t)rights, k);
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
    // #844: handle_get takes a snapshot + HOLDS a ref on the obj (k) under the
    // handle-table lock, so the ref is live across the blocking kobj_irq_wait
    // even if a sibling thread closes this slot concurrently. This subsumes
    // the old explicit kobj_irq_ref/unref borrow (R9 F143): the snapshot's ref
    // IS the borrow now. handle_put on every exit drops it. The (hidx_t)hraw
    // cast saturates large u64 to negative, which handle_get rejects (h < 0).
    struct Handle hh;
    if (handle_get(p, (hidx_t)hraw, &hh) < 0)        return -1;
    if (hh.kind != KOBJ_IRQ)               { handle_put(&hh); return -1; }

    // RIGHT_SIGNAL gates waits on KObj_IRQ — a holder without SIGNAL can pass
    // the handle around (future Phase 5+ transfer) but not consume IRQs.
    if ((hh.rights & RIGHT_SIGNAL) == 0)   { handle_put(&hh); return -1; }

    struct KObj_IRQ *k = (struct KObj_IRQ *)hh.obj;
    if (!k)                                { handle_put(&hh); return -1; }

    u32 count = kobj_irq_wait(k);
    handle_put(&hh);
    // RW-7 R1-F1: a 2nd concurrent waiter on the single-waiter KObj_IRQ is
    // refused (would otherwise extinct the kernel at sleep's single-waiter
    // assert). Surface it as the same -1 error a bad handle returns.
    if (count == KOBJ_IRQ_WAIT_BUSY) return -1;
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

    // #844: handle_get snapshots the slot + HOLDS a ref on the obj (km) under
    // the handle-table lock, so km cannot be freed by a sibling handle_close
    // between the read and burrow_create_mmio (which takes its OWN kobj_mmio
    // ref). The ref is held until handle_put below so km->size stays valid
    // across burrow_map. handle_put on EVERY exit path.
    struct Handle hh;
    if (handle_get(p, (hidx_t)hraw, &hh) < 0)        return -1;
    if (hh.kind != KOBJ_MMIO)              { handle_put(&hh); return -1; }
    if ((hh.rights & RIGHT_MAP) == 0)      { handle_put(&hh); return -1; }

    // Bound requested prot by the handle's rights. R+W → handle must have
    // RIGHT_WRITE; R → handle must have RIGHT_READ. EXEC is rejected entirely
    // for MMIO mappings (device-memory PTEs aren't usefully executable).
    u32 prot = (u32)prot_raw;
    if (prot == 0)                                   { handle_put(&hh); return -1; }
    if (prot & ~(u32)(VMA_PROT_READ | VMA_PROT_WRITE)) { handle_put(&hh); return -1; }
    if ((prot & VMA_PROT_WRITE) && !(hh.rights & RIGHT_WRITE)) { handle_put(&hh); return -1; }
    if ((prot & VMA_PROT_READ)  && !(hh.rights & RIGHT_READ))  { handle_put(&hh); return -1; }

    // R10 F155 (P2) close: AArch64 has no write-only AP encoding
    // (AP[2:1] = {00=RW EL1, 01=RW any, 10=RO EL1, 11=RO any} — no
    // W-only state per ARM ARM D5.4.1). A `prot=VMA_PROT_WRITE` only
    // request would result in a fully-RW PTE, breaking the rights
    // claim ("caller can write but not read this device"). Reject the
    // construct so the rights model and the actual PTE always agree.
    if ((prot & VMA_PROT_WRITE) && !(prot & VMA_PROT_READ)) { handle_put(&hh); return -1; }

    struct KObj_MMIO *km = (struct KObj_MMIO *)hh.obj;
    if (!km)                               { handle_put(&hh); return -1; }
    if (km->magic != KOBJ_MMIO_MAGIC)      { handle_put(&hh); return -1; }

    // Create the Burrow. handle_count=1 is the construction reference.
    struct Burrow *b = burrow_create_mmio(km);
    if (!b)                                { handle_put(&hh); return -1; }

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
        handle_put(&hh);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->vma_lock);
    handle_put(&hh);
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

    // I-34 CreateBegin (specs/allowance.tla): a NARROWED allowance bounds the
    // per-buffer DMA size (dma_max; 0 = no DMA permitted). A broad (NULL)
    // allowance -- the warden + the trusted servers -- passes (bounded by
    // KOBJ_DMA_MAX_SIZE below). The cumulative per-driver DMA-pool budget is a
    // documented v1.x refinement composing with the #65 resource floor: it is
    // the resource-DoS axis, not the I-34 cross-device-authority axis (DMA
    // buffers are the driver's OWN kernel memory, never another device's regs).
    if (!allowance_permits(p, HW_RES_DMA, size, 0))  return -1;

    struct KObj_DMA *k = kobj_dma_create((size_t)size);
    if (!k)                                          return -1;

    // I-34 CreateCommit: install under the allowance re-check (revoke race).
    hidx_t h = allowance_handle_alloc(p, KOBJ_DMA, (rights_t)rights, k);
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

    // #844: handle_get snapshots the slot + HOLDS a ref on the obj (kd) under
    // the handle-table lock; the ref bridges the read -> burrow_create_dma
    // (which takes its own kobj_dma ref) and keeps kd->size / kd->pa valid
    // across burrow_map. handle_put on EVERY exit path.
    struct Handle hh;
    if (handle_get(p, (hidx_t)hraw, &hh) < 0)        return -1;
    if (hh.kind != KOBJ_DMA)               { handle_put(&hh); return -1; }
    if ((hh.rights & RIGHT_MAP) == 0)      { handle_put(&hh); return -1; }

    // Bound requested prot by the handle's rights. EXEC is rejected
    // unconditionally — DMA buffers carry data, never code (W^X invariant
    // I-12 + structural defense against ELF-loaded code executing from DMA).
    u32 prot = (u32)prot_raw;
    if (prot == 0)                                   { handle_put(&hh); return -1; }
    if (prot & ~(u32)(VMA_PROT_READ | VMA_PROT_WRITE)) { handle_put(&hh); return -1; }
    if ((prot & VMA_PROT_WRITE) && !(hh.rights & RIGHT_WRITE)) { handle_put(&hh); return -1; }
    if ((prot & VMA_PROT_READ)  && !(hh.rights & RIGHT_READ))  { handle_put(&hh); return -1; }

    // AArch64 has no write-only PTE encoding (mirrors SYS_MMIO_MAP R10
    // F155): a `prot = WRITE` only request would map fully-RW, breaking
    // the rights claim. Reject so rights model and PTE always agree.
    if ((prot & VMA_PROT_WRITE) && !(prot & VMA_PROT_READ)) { handle_put(&hh); return -1; }

    struct KObj_DMA *kd = (struct KObj_DMA *)hh.obj;
    if (!kd)                               { handle_put(&hh); return -1; }
    if (kd->magic != KOBJ_DMA_MAGIC)       { handle_put(&hh); return -1; }

    // Create the Burrow. handle_count=1 is the construction reference.
    struct Burrow *b = burrow_create_dma(kd);
    if (!b)                                { handle_put(&hh); return -1; }

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
        handle_put(&hh);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->vma_lock);

    // PA fits in 40 bits at v1.0 (TCR.IPS bound; mmu.c:668). The s64 cast is
    // safe — no valid PA has the sign bit set. Read kd->pa before handle_put
    // (kd is also kept alive by burrow_create_dma's ref, but read it while we
    // still demonstrably hold a ref).
    s64 pa = (s64)kd->pa;
    handle_put(&hh);
    return pa;
}

// Forward decl: the common user-VA range check (defined below) -- SYS_PCI_INFO
// copies a struct out, so it needs the bound check before its definition site.
static bool sys_validate_user_buf(u64 buf_va, u64 len);

// =============================================================================
// SYS_PCI_CLAIM — claim a VirtIO-PCI function as a KObj_PCI handle (pci-1c).
// =============================================================================
//
// AArch64 ABI: x0 = virtio_device_id.
//
// Cap-gated (CAP_HW_CREATE) + (bus,dev,fn)-exclusive, exactly like
// SYS_MMIO_CREATE. Mints a KOBJ_PCI handle with FIXED rights R|W|MAP -- a
// device owner always needs read + write + map, and KObj_PCI is
// non-transferable (I-5; KOBJ_KIND_HW_MASK), so the claimer IS the driver and
// there is no partial-rights / transfer use case. (This is the deliberate
// asymmetry vs SYS_MMIO_CREATE, whose rights are caller-supplied because a
// future 9P transfer path could narrow them.) Returns hidx_t (>= 0) on
// success, -1 on EPERM / device-not-found / already-claimed / BAR-assign
// failure / malformed-cap-list / OOM / table-full.
s64 sys_pci_claim_handler(u64 virtio_device_id, u64 a1) {
    (void)a1;
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // CAP_HW_CREATE, ACQUIRE-load (proc_become_legate is a cross-thread caps
    // writer; mirrors sys_mmio_create_handler).
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;

    // A VIRTIO device id is a u16 on the wire; reject anything wider so the
    // (u32) narrowing below cannot alias a real id.
    if (virtio_device_id > (u64)0xFFFFFFFFu)         return -1;

    // I-34 CreateBegin (specs/allowance.tla; build-arc step 6): SYS_PCI_CLAIM is
    // the fourth hw-handle-minting path. Resolve the device id -> (bus,dev,fn)
    // read-only (the SAME first match kobj_pci_claim will pick), then gate it
    // against the calling Proc's per-(bus,dev,fn) PCI allowance axis. A broad
    // Proc (the warden + the trusted servers, allowance == NULL) passes; a
    // NARROWED driver may claim only a function the warden conferred -- closing
    // the bypass where a driver narrowed to one device could claim another's PCI
    // function ("a PCI device's allowance IS its claimed BARs", MENAGERIE.md §4).
    // Gating on the resolved bdf BEFORE the claim means a not-permitted device
    // is never enabled (MEM-decode + bus-master) only to be rolled back.
    u8 bus, dev, fn;
    if (kobj_pci_resolve_bdf((u32)virtio_device_id, &bus, &dev, &fn) != 0)
        return -1;
    if (!allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(bus, dev, fn), 0))
        return -1;

    struct KObj_PCI *k = kobj_pci_claim((u32)virtio_device_id);
    if (!k)                                          return -1;

    // I-34 CreateCommit: install through the allowance gate, re-checking the
    // `revoked` flag UNDER the allowance lock proc_revoke_allowance takes -- so a
    // DeviceRemoved racing the claim aborts here (the in-flight create loses the
    // race) rather than leaving a live KObj_PCI handle over a revoked allowance
    // (allowance.tla revoke_race / BUGGY_COMMIT_NO_RECHECK). A broad Proc's
    // allowance_handle_alloc is plain handle_alloc. Fixed R|W|MAP, NO TRANSFER:
    // KOBJ_PCI is in KOBJ_KIND_HW_MASK, so handle_dup + the 9P path reject it (I-5).
    hidx_t h = allowance_handle_alloc(p, KOBJ_PCI,
                            (rights_t)(RIGHT_READ | RIGHT_WRITE | RIGHT_MAP), k);
    if (h < 0) {
        // Roll back the claim so the (bus,dev,fn) slot + BAR PA claims free for
        // a retry / another driver (mirrors the SYS_MMIO_CREATE rollback).
        kobj_pci_unref(k);
        return -1;
    }
    return (s64)h;
}

// =============================================================================
// SYS_PCI_MAP_BAR — map a KObj_PCI handle's BAR into user VA (pci-1c).
// =============================================================================
//
// AArch64 ABI: x0 = handle, x1 = vaddr, x2 = bar_index, x3 = prot.
//
// Mirrors SYS_MMIO_MAP: validates the handle (KOBJ_PCI + RIGHT_MAP), bounds
// prot by the handle rights (R+W needs RIGHT_WRITE; EXEC rejected; W-without-R
// rejected -- AArch64 has no W-only AP), resolves bar_index -> the BAR's
// KObj_MMIO, wraps it in a BURROW_TYPE_MMIO Burrow, installs the VMA under
// p->vma_lock, and drops the construction ref. Returns 0 / -1.
s64 sys_pci_map_bar_handler(u64 hraw, u64 vaddr, u64 bar_index, u64 prot_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;

    // #844: handle_get snapshots the slot + HOLDS a ref on the KObj_PCI (k) under
    // the handle-table lock, so a sibling handle_close cannot free k between the
    // read and burrow_create_mmio. The held k keeps its bars[].mmio alive (k owns
    // that kobj_mmio ref); burrow_create_mmio takes its OWN ref (#847 dual
    // lifetime), so the mapping survives even after handle_put drops k's snapshot.
    struct Handle hh;
    if (handle_get(p, (hidx_t)hraw, &hh) < 0)        return -1;
    if (hh.kind != KOBJ_PCI)              { handle_put(&hh); return -1; }
    if ((hh.rights & RIGHT_MAP) == 0)     { handle_put(&hh); return -1; }

    // Bound prot by the handle rights; reject EXEC and the W-without-R construct
    // (identical to sys_mmio_map -- device memory, no W-only AP encoding).
    u32 prot = (u32)prot_raw;
    if (prot == 0)                                   { handle_put(&hh); return -1; }
    if (prot & ~(u32)(VMA_PROT_READ | VMA_PROT_WRITE)) { handle_put(&hh); return -1; }
    if ((prot & VMA_PROT_WRITE) && !(hh.rights & RIGHT_WRITE)) { handle_put(&hh); return -1; }
    if ((prot & VMA_PROT_READ)  && !(hh.rights & RIGHT_READ))  { handle_put(&hh); return -1; }
    if ((prot & VMA_PROT_WRITE) && !(prot & VMA_PROT_READ)) { handle_put(&hh); return -1; }

    struct KObj_PCI *k = (struct KObj_PCI *)hh.obj;
    if (!k)                               { handle_put(&hh); return -1; }
    if (k->magic != KOBJ_PCI_MAGIC)       { handle_put(&hh); return -1; }

    // Bound bar_index in u64 width BEFORE the u32 narrowing: a bar_index >= 2^32
    // whose low dword is a valid index would otherwise alias a real BAR. >= 6 is
    // rejected here; kobj_pci_bar_mmio re-checks (defense in depth) + rejects an
    // absent BAR.
    if (bar_index >= PCI_BAR_COUNT)       { handle_put(&hh); return -1; }
    struct KObj_MMIO *km = kobj_pci_bar_mmio(k, (u32)bar_index);
    if (!km)                              { handle_put(&hh); return -1; }
    if (km->magic != KOBJ_MMIO_MAGIC)     { handle_put(&hh); return -1; }

    struct Burrow *b = burrow_create_mmio(km);
    if (!b)                               { handle_put(&hh); return -1; }

    // burrow_map walks + splices p->vmas, so it holds p->vma_lock (the #713
    // discipline). Lock order vma_lock -> buddy zone->lock holds. km->size is the
    // full decoded BAR size; the user maps the whole BAR and indexes the
    // VIRTIO_PCI_CAP regions within it.
    spin_lock(&p->vma_lock);
    int rc = burrow_map(p, b, vaddr, km->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        spin_unlock(&p->vma_lock);
        handle_put(&hh);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->vma_lock);
    handle_put(&hh);
    return 0;
}

// =============================================================================
// SYS_PCI_INFO — copy a KObj_PCI handle's resolved topology to user (pci-1c).
// =============================================================================
//
// AArch64 ABI: x0 = handle, x1 = info_va.
//
// Validates the handle (KOBJ_PCI + RIGHT_READ), builds a zero-initialized
// struct t_pci_info (no uninitialized padding leaked -- the RW-8 R2-F1 class),
// and copies it out byte-by-byte (the fd2path / getcwd idiom; uaccess has no
// bulk copy + no alignment requirement on uaccess_store_u8). Returns 0 / -1.
s64 sys_pci_info_handler(u64 hraw, u64 info_va) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if (!sys_validate_user_buf(info_va, sizeof(struct t_pci_info))) return -1;

    struct Handle hh;
    if (handle_get(p, (hidx_t)hraw, &hh) < 0)        return -1;
    if (hh.kind != KOBJ_PCI)              { handle_put(&hh); return -1; }
    if ((hh.rights & RIGHT_READ) == 0)    { handle_put(&hh); return -1; }

    struct KObj_PCI *k = (struct KObj_PCI *)hh.obj;
    if (!k)                               { handle_put(&hh); return -1; }
    if (k->magic != KOBJ_PCI_MAGIC)       { handle_put(&hh); return -1; }

    struct t_pci_info info = {0};   // zeroes every field incl. pad -> no leak
    for (u32 i = 0; i < PCI_BAR_COUNT; i++) {
        info.bars[i].pa      = k->bars[i].pa;
        info.bars[i].size    = k->bars[i].size;
        info.bars[i].present = k->bars[i].present ? 1u : 0u;
        info.bars[i].is_64   = k->bars[i].is_64   ? 1u : 0u;
    }
    for (u32 i = 0; i < VIRTIO_PCI_CAP_REGION_COUNT; i++) {
        info.regions[i].offset  = k->regions[i].offset;
        info.regions[i].length  = k->regions[i].length;
        info.regions[i].bar     = k->regions[i].bar;
        info.regions[i].present = k->regions[i].present ? 1u : 0u;
    }
    info.notify_off_multiplier = k->notify_off_multiplier;
    info.intid                 = k->intid;
    info.intid_valid           = k->intid_valid ? 1u : 0u;
    info.bus                   = k->bus;
    info.dev                   = k->dev;
    info.fn                    = k->fn;
    info.virtio_device_id      = k->virtio_device_id;

    const u8 *src = (const u8 *)&info;
    for (u64 i = 0; i < sizeof(struct t_pci_info); i++) {
        if (uaccess_store_u8(info_va + i, src[i]) != 0) { handle_put(&hh); return -1; }
    }
    handle_put(&hh);
    return 0;
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

// Helper: look up an open KOBJ_SPOOR handle, validate rights. Returns a
// REF-HELD Spoor on success (NULL on bad fd / wrong kind / missing rights).
//
// #844: handle_get bumps the Spoor's refcount under the handle-table lock; that
// ref is TRANSFERRED to the caller, who MUST spoor_clunk() the returned Spoor
// when done. The borrow keeps the Spoor alive for the duration of the caller's
// use even if a sibling thread closes the fd concurrently (the old contract
// returned a bare borrowed pointer into the live table -- a TOCTOU UAF in a
// multi-threaded Proc). The obj is always a Spoor here (kind-gated), so
// handle_get's acquire was spoor_ref and the caller balances with spoor_clunk.
static struct Spoor *sys_lookup_spoor(struct Proc *p, hidx_t h, rights_t required) {
    struct Handle hh;
    if (handle_get(p, h, &hh) < 0)                   return NULL;
    if (hh.kind != KOBJ_SPOOR ||
        (hh.rights & required) != required) {
        handle_put(&hh);                             // drop the ref handle_get took
        return NULL;
    }
    return (struct Spoor *)hh.obj;                   // ref TRANSFERRED to caller
}

// Helper: look up an open r/w-capable handle (KOBJ_SPOOR only) + validate
// rights. Returns the slot pointer or NULL. Post-stalk-3c the only
// readable/writable handle kind is KOBJ_SPOOR -- a /srv connection endpoint
// is itself a KOBJ_SPOOR conn Spoor (the server side, and the CSRVCLIENT
// client side), driven through the devsrv read/write vtable. The client-side
// KObj_Srv conn handle that once bridged raw bytes here was retired with
// SYS_SRV_CONNECT; a KObj_Srv handle is now only a service listener, never a
// transport. The kind check (below) precedes any obj dereference.
// #844: returns a REF-HELD Spoor (NULL on bad fd / missing rights / wrong
// kind). Same ref-transfer contract as sys_lookup_spoor -- the caller MUST
// spoor_clunk() the result. Only KOBJ_SPOOR is read/write-able (a KOBJ_SRV
// handle is a /srv listener, not a byte stream; the client + accepted conn
// endpoints are KOBJ_SPOOR conn Spoors driven by devsrv read/write). Returning
// the ref-held Spoor (not the live slot pointer) closes the TOCTOU where the
// slot dangled across the blocking dev->read / dev->write that callers run.
static struct Spoor *sys_lookup_rw_handle(struct Proc *p, hidx_t h,
                                          rights_t required) {
    struct Handle hh;
    if (handle_get(p, h, &hh) < 0)                   return NULL;
    if ((hh.rights & required) != required ||
        hh.kind != KOBJ_SPOOR) {
        handle_put(&hh);
        return NULL;
    }
    return (struct Spoor *)hh.obj;                   // ref TRANSFERRED to caller
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
// Only KOBJ_SPOOR is writable (sys_lookup_rw_handle filters): the write
// routes through the Dev `.write` vtable (Spoor.offset). A byte-mode /srv
// connection endpoint is itself a KOBJ_SPOOR conn Spoor, so its bytes ride
// this path too -- devsrv_write picks the server arm (srvconn_server_send)
// or the CSRVCLIENT client arm (srvconn_client_send) by the conn direction.
// The client-side KObj_Srv conn handle that once routed here was retired
// with SYS_SRV_CONNECT (stalk-3c); the kernel-attached no-direct-I/O guard
// moved with it into devsrv_write.
s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf, u64 len) {
    if (!p || (!kbuf && len > 0))                    return -1;
    // #844: c is a REF-HELD Spoor (the lookup transferred the ref); it keeps c
    // alive across the blocking dev->write even if a sibling closes the fd.
    // spoor_clunk on EVERY exit after the lookup.
    struct Spoor *c = sys_lookup_rw_handle(p, h, RIGHT_WRITE);
    if (!c)                                          return -1;
    // #81: a T_OPATH navigation handle is NOT a byte-I/O channel (it is born R|W
    // for create/walk-target use but perm_check-exempt at open). Reject every
    // write, including len 0, so it cannot serve content (IDENTITY-DESIGN 9.4 #81).
    if (c->flag & CWALKONLY)                       { spoor_clunk(c); return -1; }
    if (len == 0)                                  { spoor_clunk(c); return 0; }
    if (!c->dev || !c->dev->write)                 { spoor_clunk(c); return -1; }
    long n = c->dev->write(c, kbuf, (long)len, c->offset);
    if (n < 0)                                     { spoor_clunk(c); return -1; }
    c->offset += n;
    spoor_clunk(c);
    return (s64)n;
}

// Inner — testable with kernel-side buf. Returns bytes read (>=0; 0
// on EOF) or -1 on bad handle / wrong kind / missing rights / dev error.
//
// Only KOBJ_SPOOR is readable (sys_lookup_rw_handle filters): the read
// routes through the Dev `.read` vtable (Spoor.offset). A byte-mode /srv
// connection endpoint is itself a KOBJ_SPOOR conn Spoor -- devsrv_read picks
// the server arm (srvconn_server_recv*) or the CSRVCLIENT client arm
// (srvconn_client_recv) by the conn direction. The client-side KObj_Srv conn
// handle that once routed here was retired with SYS_SRV_CONNECT (stalk-3c).
s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len) {
    if (!p || (!kbuf && len > 0))                    return -1;
    // #844: c is a REF-HELD Spoor; it stays alive across the blocking
    // dev->read even if a sibling closes the fd. spoor_clunk on EVERY exit.
    struct Spoor *c = sys_lookup_rw_handle(p, h, RIGHT_READ);
    if (!c)                                          return -1;
    // #81: a T_OPATH navigation handle is NOT a byte-I/O channel -- reject every
    // read (the perm_check-exempt O_PATH open would otherwise be a read-bypass,
    // e.g. the 0400 /system.key via /bin/system.key). IDENTITY-DESIGN 9.4 #81.
    if (c->flag & CWALKONLY)                       { spoor_clunk(c); return -1; }
    if (len == 0)                                  { spoor_clunk(c); return 0; }
    if (!c->dev || !c->dev->read)                  { spoor_clunk(c); return -1; }
    long n = c->dev->read(c, kbuf, (long)len, c->offset);
    if (n < 0)                                     { spoor_clunk(c); return -1; }
    c->offset += n;
    spoor_clunk(c);
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
        // discipline: bad fd should return -EBADF regardless of len).
        // #844: the lookup transfers a Spoor ref; release it immediately
        // (validation only -- no I/O on a zero-length write).
        struct Spoor *c0 = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_WRITE);
        if (!c0)                                     return -1;
        // #81 F1: the gate must cover the len==0 fast-path too (it short-circuits
        // before sys_write_for_proc) -- an O_PATH handle does NO byte I/O, incl. 0.
        if (c0->flag & CWALKONLY)                  { spoor_clunk(c0); return -1; }
        spoor_clunk(c0);
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
        struct Spoor *c0 = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_READ);
        if (!c0)                                     return -1;   // #844: validate + release
        // #81 F1: the gate must cover the len==0 fast-path too (it short-circuits
        // before sys_read_for_proc) -- an O_PATH handle does NO byte I/O, incl. 0.
        if (c0->flag & CWALKONLY)                  { spoor_clunk(c0); return -1; }
        spoor_clunk(c0);
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
// Non-static (declared in <thylacine/spoor.h>): the stalk resolver shares this
// stat-fetch for its per-component X-search.
int spoor_stat_native(struct Spoor *c, struct t_stat *out) {
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

    // Rights gate: KOBJ_SPOOR with RIGHT_READ (sys_lookup_rw_handle filters
    // KOBJ_SRV + checks rights). #844: c is REF-HELD; spoor_clunk on every exit
    // -- the ref keeps c alive across the (possibly blocking) dev->stat_native.
    struct Spoor *c = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_READ);
    if (!c)                                           return -1;

    // Fill a kernel-scratch t_stat from the Dev. Failing the Dev's
    // stat_native (NULL slot or error return) maps to -1 here.
    struct t_stat ks;
    if (spoor_stat_native(c, &ks) != 0)             { spoor_clunk(c); return -1; }

    // Copy out to user-VA. Per-byte uaccess_store_u8; on fault the user-VA may
    // have partially-written bytes (consistent with SYS_READ).
    const u8 *src = (const u8 *)&ks;
    for (u64 i = 0; i < sizeof(struct t_stat); i++) {
        if (uaccess_store_u8(stat_va + i, src[i]) != 0) { spoor_clunk(c); return -1; }
    }
    spoor_clunk(c);
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

    // No rights mask: lseek manipulates the per-Spoor cursor, not content.
    // #844: c is REF-HELD (sys_lookup_rw_handle kind-gates to KOBJ_SPOOR +
    // RIGHT 0); the ref keeps c alive across the SEEK_END dev->stat_native
    // (which may block). spoor_clunk on EVERY exit below.
    struct Spoor *c = sys_lookup_rw_handle(p, (hidx_t)hraw, 0);
    if (!c)                                           return -1;

    // Reject lseek on a non-seekable Dev (POSIX lseek(2) on a pipe -> ESPIPE).
    // RW-4 R2-F2: the old `dev->stat_native == NULL` heuristic broke when #957
    // (devsrv) + A-4b (devproc) gave non-seekable Devs a .stat_native for fstat,
    // regressing lseek to succeed on an offset their read/write ignore. The
    // explicit dev->seekable flag (devramfs + dev9p only) decouples fstat-ability
    // from seekability.
    if (!c->dev->seekable)                          { spoor_clunk(c); return -1; }

    s64 offset = (s64)offset_raw;
    s64 new_off;

    switch (whence_raw) {
    case T_SEEK_SET:
        if (offset < 0)                             { spoor_clunk(c); return -1; }
        new_off = offset;
        break;
    case T_SEEK_CUR: {
        s64 cur = c->offset;
        if (offset > 0 && cur > INT64_MAX - offset) { spoor_clunk(c); return -1; }
        if (offset < 0 && cur < INT64_MIN - offset) { spoor_clunk(c); return -1; }
        new_off = cur + offset;
        if (new_off < 0)                            { spoor_clunk(c); return -1; }
        break;
    }
    case T_SEEK_END: {
        struct t_stat ks;
        if (spoor_stat_native(c, &ks) != 0)         { spoor_clunk(c); return -1; }
        s64 size = (s64)ks.size;
        if (size < 0)                               { spoor_clunk(c); return -1; }
        if (offset > 0 && size > INT64_MAX - offset){ spoor_clunk(c); return -1; }
        if (offset < 0 && size < INT64_MIN - offset){ spoor_clunk(c); return -1; }
        new_off = size + offset;
        if (new_off < 0)                            { spoor_clunk(c); return -1; }
        break;
    }
    default:
        spoor_clunk(c);
        return -1;
    }

    c->offset = new_off;
    spoor_clunk(c);
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
    if (!rx)                                       { spoor_clunk(tx); return -1; }
    // #844: tx + rx are REF-HELD (sys_lookup_spoor transferred a ref each). The
    // adapter takes its OWN independent ref below; we then release the two
    // lookup borrows here (UNCONDITIONAL -- each lookup ref'd, even when
    // rx==tx). The adapter ref + the fds' own handle-table refs keep tx/rx
    // alive for the rest, so every existing error path's adapter rollback
    // (spoor_unref) + the success path stay correct without further borrow
    // bookkeeping.
    spoor_ref(tx);
    if (rx != tx) spoor_ref(rx);
    spoor_clunk(tx);
    spoor_clunk(rx);

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

    // Look up the connection endpoint. stalk-3b-β retargeted this from a
    // KObj_Srv connection handle to a KOBJ_SPOOR devsrv byte-conn Spoor (the
    // product of devsrv_open's byte-mode connect, dc='s' + SRV_CONN_MAGIC aux,
    // resolved via devsrv_conn_of). Rights: READ + WRITE (the kernel 9P client
    // both reads and writes through the byte rings). The CSRVCLIENT flag gates
    // it to a CLIENT endpoint -- attaching a SERVER endpoint (from
    // SYS_SRV_ACCEPT) would drive the rings the wrong way.
    // #844: snapshot + HOLD the Spoor ref across srvconn_attach_dev9p_root,
    // which takes its OWN srvconn_ref on the embedded SrvConn. handle_put on
    // EVERY exit below.
    struct Handle hh;
    if (handle_get(p, (hidx_t)srv_fd_raw, &hh) < 0)  return -1;
    if (hh.kind != KOBJ_SPOOR)             { handle_put(&hh); return -1; }
    if (!hh.obj)                           { handle_put(&hh); return -1; }
    if ((hh.rights & (RIGHT_READ | RIGHT_WRITE))
        != (RIGHT_READ | RIGHT_WRITE))     { handle_put(&hh); return -1; }
    struct Spoor *conn_spoor = (struct Spoor *)hh.obj;
    if (!(conn_spoor->flag & CSRVCLIENT))  { handle_put(&hh); return -1; }   // client endpoint only
    struct SrvConn *cn = devsrv_conn_of(conn_spoor);
    if (!cn)                               { handle_put(&hh); return -1; }   // not a devsrv conn Spoor

    // Byte-mode gate. A byte-conn Spoor's SrvConn is byte_mode (devsrv_open set
    // it from the service mode). A non-byte SrvConn cannot reach here (only a
    // byte-mode service yields a conn Spoor; 9p-mode yields a dev9p root that
    // never carries SRV_CONN_MAGIC), but re-check defensively -- a second
    // p9_client over a 9p-mode SrvConn's rings would interleave frames + corrupt
    // the wire. Atomic acquire pairs srvconn_set_byte_mode's RELEASE.
    if (!__atomic_load_n(&cn->byte_mode, __ATOMIC_ACQUIRE))
                                         { handle_put(&hh); return -1; }

    // Copy aname into kernel scratch. Same shape as SYS_ATTACH_9P.
    u8 aname_scratch[SYS_ATTACH_ANAME_MAX];
    for (u64 i = 0; i < aname_len; i++) {
        if (uaccess_load_u8(aname_va + i, &aname_scratch[i]) != 0)
            { handle_put(&hh); return -1; }
    }

    // Wrap the SrvConn's CLIENT side into a dev9p root via the shared helper
    // (srvconn_attach_dev9p_root, stalk-3b-β): the audited transport-init +
    // kernel_attached(early, R1 F4) + handshake-deadline(R1 F1) + Tversion/
    // Tattach + install + attached_owner-stamp + rollback. The helper drops the
    // p9_attached construction ref before returning, so the returned root owns
    // the session via its dev9p_priv->attached_owner. devsrv_open's 9p-mode
    // connect shares the SAME helper -- the 9P-unification.
    int aerr = 0;
    struct Spoor *root = srvconn_attach_dev9p_root(
        cn, aname_len > 0 ? aname_scratch : NULL, aname_len, p->principal_id, &aerr);
    if (!root) {
        // A-3c/M6: surface the Tattach Rlerror ecode (e.g. -T_E_ACCES on a
        // per-user stratumd dataset-scope refusal) rather than a bare -1.
        handle_put(&hh);
        return attach_err_to_ret(aerr);
    }

    // Install the dev9p root as a KOBJ_SPOOR handle. On failure, spoor_clunk
    // (root) runs dev9p_close -> the last attached_owner unref -> session
    // teardown (the construction ref was already dropped inside the helper).
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR,
                             RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER, root);
    if (fd < 0) {
        spoor_clunk(root);
        handle_put(&hh);
        return -1;
    }
    // #844: borrow done -- the attach session now owns conn_spoor's SrvConn
    // (its own srvconn_ref); drop the syscall's borrow on conn_spoor.
    handle_put(&hh);
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

    // territory_pivot_root handles: NULL-source rejection, no-current-root
    // rejection (the pivot pre-condition), idempotent same-pointer, prior-root
    // displacement via spoor_clunk, spoor_ref of the new.
    // #844: source is REF-HELD (a borrow); territory_pivot_root takes its OWN
    // ref for the root_spoor, so release the borrow after.
    int rc = territory_pivot_root(p->territory, source);
    spoor_clunk(source);
    return rc == 0 ? 0 : -1;
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
    // Both paths return a REF-HELD src: the handle arm via sys_lookup_spoor's
    // #844 by-value snapshot, the FROM_ROOT arm via RW-4 SA-F1's
    // territory_root_ref (atomic read+ref under ns_lock). Every exit path below
    // spoor_clunks src exactly once. spoor_clone(src) mints the new Spoor for
    // the result fd.
    struct Spoor *src;
    if (spoor_fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
        if (!p->territory)                            return -1;
        // RW-4 SA-F1: atomic read+ref under the Territory ns_lock. The prior
        // read-then-spoor_ref left a UAF window: a concurrent pivot_root could
        // swap root_spoor + clunk the old one to zero between the read and the
        // ref. territory_root_ref closes it.
        src = territory_root_ref(p->territory);
        if (!src)                                     return -1;
    } else {
        src = sys_lookup_spoor(p, (hidx_t)spoor_fd_raw, RIGHT_READ);   // ref-held
        if (!src)                                     return -1;
    }

    // #957: cross the SOURCE if it is a mount point (Plan 9 domount on the
    // directory we walk THROUGH) -- walk INTO + X-check the mounted root, not
    // the shadowed mount point. Mirrors stalk's base cross so a single-hop
    // SYS_WALK_OPEN behaves exactly like a one-component stalk. The crossed
    // clone is OWNED (its own fid); clunk the original src and adopt it. In
    // practice every fd reaching here is already post-cross (the result cross
    // below crosses every walk/open output) and the Territory root is never a
    // mount-table entry -- so this is a no-op for current callers -- but it is
    // correct if a mount-point fd ever exists. stalk_cross_mounts uses only
    // src's identity (dc/devno/qid), not src->dev, so it precedes the dev-check.
    {
        struct Spoor *crossed = NULL;
        if (stalk_cross_mounts(p, src, &crossed) < 0)    { spoor_clunk(src); return -1; }
        if (crossed) { spoor_clunk(src); src = crossed; }
    }
    if (!src->dev || !src->dev->walk || !src->dev->open) { spoor_clunk(src); return -1; }

    // A-2d (IDENTITY-DESIGN.md 3.7.1): search (X) permission on the source
    // directory before walking into it. Gated on the Dev's perm_enforced flag
    // (dev9p deferred to A-3; devramfs live). devramfs dirs are 0555, so the
    // PRINCIPAL_SYSTEM owner traverses while a non-system principal needs
    // other-x. fail-closed if the Dev cannot vouch for the metadata. This is
    // additive to the handle-RIGHT gate above (capability axis); both must hold.
    if (src->dev->perm_enforced) {
        struct t_stat src_st;
        if (spoor_stat_native(src, &src_st) != 0)        { spoor_clunk(src); return -1; }
        if (perm_check(p, &src_st, PERM_X) != 0)         { spoor_clunk(src); return -1; }
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
        if (uaccess_load_u8(name_va + i, &b) != 0)    { spoor_clunk(src); return -1; }
        if (b == '/' || b == '\0')                    { spoor_clunk(src); return -1; }
        name_scratch[i] = (char)b;
    }
    if (name_len_raw == 1 && name_scratch[0] == '.')  { spoor_clunk(src); return -1; }
    if (name_len_raw == 2 && name_scratch[0] == '.' &&
                              name_scratch[1] == '.') { spoor_clunk(src); return -1; }
    // Unconditional NUL terminator — REQUIRED for dev9p_walk's strlen scan.
    name_scratch[name_len_raw] = '\0';

    // Clone the source Spoor — gives us an independent cursor whose aux
    // dev->walk will replace with a freshly-allocated priv (carrying the
    // new fid). The clone starts at ref=1; spoor_clunk on failure runs
    // dev->close (clunks the fid if walk had progressed) + drops the ref.
    struct Spoor *nc = spoor_clone(src);
    if (!nc)                                          { spoor_clunk(src); return -1; }

    // Issue the walk. Pack the single name + length into one-element
    // arrays for the dev vtable's nname-style signature. dev9p_walk
    // allocates a fresh fid + drives p9_client_walk + replaces nc->aux
    // with new_priv (fid_owned=true); on failure it clunks the fid + frees
    // the Walkqid carrier itself.
    const char *names[1] = { name_scratch };
    struct Walkqid *w = src->dev->walk(src, nc, names, 1);
    // #844: src's last use is the walk above; release the borrow now. nc
    // carries the walk result (its own ref) through the rest of the handler,
    // so the post-walk exits clunk nc (unchanged), not src.
    spoor_clunk(src);
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

    // #66: append the walked component to nc's namespace name. nc SHARES src's
    // Path (from spoor_clone); spoor_path_extend reads that shared Path as the
    // base and installs the extended one. src was released above, but nc holds
    // the shared Path ref, so it is alive. Non-load-bearing (I-33) -- an OOM
    // leaves nc->path NULL. (`.`/`..` are rejected as components above, so this
    // is always a real name.) Done before the result-cross so a mount-point nc
    // carries the walked name, which stalk_cross_mounts then transplants onto
    // the mounted root.
    spoor_path_extend(nc, name_scratch, name_len_raw);

    // #957: cross the walked RESULT. If nc is a mount point, yield the MOUNTED
    // ROOT (Plan 9 domount on the resolved node), so a single-hop SYS_WALK_OPEN
    // onto a mount point opens/returns the mounted tree -- identical to stalk's
    // final-element cross (kernel/stalk.c) + SYS_OPEN. THE FIX: libthyla-rs fs::
    // navigates parent dirs component-by-component via SYS_WALK_OPEN
    // (file::with_parent_dir), so without this a create/rename/unlink into a
    // per-user /home/<user> 9P mount resolved the shadowed SYSTEM-owned
    // placeholder and denied the write (the owner -- the logged-in user -- saw
    // `other` on the placeholder's 0755). The crossed clone is OWNED (own fid);
    // the perm_check + Dev.open below then run on the mounted root, and the
    // installed handle's rights are derived from it.
    {
        struct Spoor *crossed = NULL;
        if (stalk_cross_mounts(p, nc, &crossed) < 0)  { spoor_clunk(nc); return -1; }
        if (crossed) { spoor_clunk(nc); nc = crossed; }
    }

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
        // FS-delta 9.4) but NOT from the src X-search above. #957-audit F3:
        // after the result-cross `nc` may be a crossed MOUNTED ROOT with a
        // DIFFERENT Dev than src (a per-user dev9p session, or the devsrv->dev9p
        // boundary), so read `perm_enforced` + stat off `nc` itself -- the node
        // actually opened -- not off src.
        if (nc->dev->perm_enforced) {
            struct t_stat nc_st;
            if (spoor_stat_native(nc, &nc_st) != 0)  { spoor_clunk(nc); return -1; }
            if (perm_check(p, &nc_st, perm_want_for_omode((u32)omode_raw)) != 0) {
                spoor_clunk(nc);
                return -1;
            }
        }
        // Issue the open. Dev.open returns EITHER nc opened in place (dev9p /
        // devramfs: state mutated -- COPEN, mode/offset reset, qid refreshed)
        // OR a DIFFERENT owned Spoor that REPLACES nc (devsrv open=connect on a
        // /srv/<name> leaf: the service-ref is consumed + the connection
        // endpoint returned). #957-audit F1: the single-hop walk now crosses
        // into /srv, so File::open("/srv/<name>") (non-O_PATH) reaches a service
        // leaf here -- this handler MUST adopt the returned Spoor like stalk
        // (kernel/stalk.c) does, else it installs the spent service-ref + leaks
        // the connection endpoint + its SrvConn + a poster backlog slot. On
        // failure nc still has its own walk-allocated fid; spoor_clunk runs
        // dev->close -> p9_client_clunk on that fid + frees the priv.
        struct Spoor *opened = nc->dev->open(nc, (int)omode_raw);
        if (!opened) {
            spoor_clunk(nc);
            return -1;
        }
        if (opened != nc) {
            // #66 (audit F2): transplant the walked name onto the connection
            // endpoint (mirrors the stalk adoption arm) so fd2path reports the
            // path the caller opened, not the conn root's "/". `opened` is
            // thread-local pre-install (I-33 set-before-publish). Non-load-bearing.
            spoor_path_transplant(opened, nc);
            spoor_clunk(nc);   // the old nc (service-ref) is spent; open did not consume it
            nc = opened;       // adopt the connection endpoint (one owned ref)
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
    // This is the one case NOT derived from omode & 3. #81: "not a byte-I/O
    // channel" is now ENFORCED -- the CWALKONLY flag (set below) makes
    // sys_read/write/readdir reject it, so the perm_check-exempt O_PATH open
    // cannot be a read-bypass (it once leaked the 0400 /system.key).
    rights_t r;
    if (omode_raw & SYS_WALK_OPEN_OPATH) {
        r = RIGHT_READ | RIGHT_WRITE;
        nc->flag |= CWALKONLY;   // #81: a navigation handle -- sys_read/write/readdir reject it
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
// SYS_OPEN — the multi-component pathname open (stalk-1; A-5b-0;
// docs/STALK-DESIGN.md). Generalizes SYS_WALK_OPEN: rather than a single
// component, it resolves a full '/'-separated path through the `stalk` resolver
// (per-component X-search, '.'/'..' contained at the base, one Dev at v1.0 --
// mount-crossing is stalk-2). The arg validation + rights derivation mirror
// sys_walk_open_handler; the resolution itself is stalk().
// =============================================================================
// SYS_CHDIR(path, len) -- set the per-Proc cwd (LS-4; LIFE-SUPPORT.md LS-4).
// Resolves `path` against the current cwd (relative) or the Territory root
// (absolute), requires the target to be a directory the caller can SEARCH (X),
// then swaps the Territory dot_path. dot is shared by a Proc's threads and
// inherited by children at spawn.
static s64 sys_chdir_handler(u64 path_va, u64 path_len_raw, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p || !p->territory) return -1;
    if (path_len_raw == 0)                           return -1;
    if (path_len_raw > SYS_OPEN_PATH_MAX)            return -1;
    if (!sys_validate_user_buf(path_va, path_len_raw)) return -1;

    char path_scratch[SYS_OPEN_PATH_MAX + 1];
    for (u64 i = 0; i < path_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(path_va + i, &b) != 0)   return -1;
        if (b == '\0')                               return -1;   // no embedded NUL
        path_scratch[i] = (char)b;
    }
    path_scratch[path_len_raw] = '\0';

    // Resolve + clean against the cwd (relative) or root (absolute).
    char cleaned[SYS_OPEN_PATH_MAX + 1];
    int cl = territory_resolve_cwd(p->territory, path_scratch, path_len_raw,
                                   cleaned, sizeof(cleaned));
    if (cl < 0)                                      return -1;

    // Resolve the cleaned absolute path from the Territory root to verify it
    // exists, is a directory, and the caller holds X (search). stalk borrows
    // root (never refs/clunks it); RW-4 SA-F1: territory_root_ref takes the ref
    // ATOMICALLY under ns_lock (a plain read-then-ref raced a concurrent
    // pivot_root's swap+clunk-to-zero). Released at the uniform exit clunk below.
    struct Spoor *root = territory_root_ref(p->territory);
    if (!root)                                       return -1;
    // `cleaned` is already an absolute, lexically-resolved path (no `.`/`..`
    // remain), so stalk's own `..` clamp at root_spoor is a redundant safety net
    // here -- the containment was already established by cwd_lexical_resolve.
    struct Spoor *q = stalk(p, root, cleaned, (u64)cl, STALK_WALK, 0);
    spoor_clunk(root);
    if (!q)                                          return -1;

    s64 rc = -1;
    if (q->qid.type & QTDIR) {
        int ok = 1;
        // Mirror stalk's gating: a perm_enforced Dev gates the search on X for
        // the caller's principal; a non-enforced Dev has no rwx to check.
        if (q->dev && q->dev->perm_enforced) {
            struct t_stat st;
            ok = (spoor_stat_native(q, &st) == 0 && perm_check(p, &st, PERM_X) == 0);
        }
        if (ok) rc = territory_setdot(p->territory, cleaned);
    }
    spoor_clunk(q);
    return rc;
}

// SYS_GETCWD(buf, len) -- copy the per-Proc cwd into the user buffer (LS-4),
// NUL-terminated. Returns the path length (excluding NUL), or -1 if the path +
// NUL does not fit `len`.
static s64 sys_getcwd_handler(u64 buf_va, u64 buf_len_raw, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p || !p->territory) return -1;
    if (buf_len_raw == 0)                            return -1;
    if (buf_len_raw > SYS_OPEN_PATH_MAX + 1)         return -1;
    if (!sys_validate_user_buf(buf_va, buf_len_raw)) return -1;

    char scratch[SYS_OPEN_PATH_MAX + 1];
    int len = territory_getdot(p->territory, scratch, sizeof(scratch));
    if (len < 0)                                     return -1;
    if ((u64)len + 1 > buf_len_raw)                  return -1;   // path + NUL must fit

    for (int i = 0; i <= len; i++) {                 // include the trailing NUL
        if (uaccess_store_u8(buf_va + (u64)i, (u8)scratch[i]) != 0) return -1;
    }
    return (s64)len;
}

// =============================================================================
// SYS_FD2PATH — return the namespace name a fd was reached by (#66; the Plan 9
// fd2path(2)). Copies the fd's Spoor's Path string (+ trailing NUL) into the
// user buffer and returns the length (excluding NUL). The fd must be a
// KOBJ_SPOOR handle the caller holds (rights == 0: NO specific access right --
// the name is of something the caller already opened, not new authority). A
// Spoor with no known name (NULL Path -- a nameless attach root, or a walk from
// a nameless fd) yields a 0-length result ("unknown"), NEVER an error: a valid
// path always begins with '/' (len >= 1), so len == 0 unambiguously means
// unknown. The Path is non-load-bearing (I-33) and immutable while the Spoor is
// ref-held (set-before-publish), so it is read locklessly with no path_ref.
// =============================================================================

static s64 sys_fd2path_handler(u64 fd_raw, u64 buf_va, u64 buf_len_raw, u64 a3) {
    (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    if (buf_len_raw == 0)                            return -1;
    if (buf_len_raw > SYS_OPEN_PATH_MAX + 1)         return -1;
    if (!sys_validate_user_buf(buf_va, buf_len_raw)) return -1;

    // #844: sys_lookup_spoor TRANSFERS the ref -- spoor_clunk on every exit.
    // rights == 0 -> any KOBJ_SPOOR handle (no access right required).
    struct Spoor *c = sys_lookup_spoor(p, (hidx_t)fd_raw, 0);
    if (!c)                                          return -1;

    // c is ref-held, so c->path (if non-NULL) is alive + immutable (I-33). Read
    // its length + bytes directly. NULL Path -> 0 ("unknown"), still a success.
    struct Path *pp = c->path;
    u32 len = pp ? pp->len : 0u;
    if ((u64)len + 1 > buf_len_raw)                 { spoor_clunk(c); return -1; }   // path + NUL must fit

    for (u32 i = 0; i < len; i++) {
        if (uaccess_store_u8(buf_va + (u64)i, (u8)pp->s[i]) != 0) { spoor_clunk(c); return -1; }
    }
    if (uaccess_store_u8(buf_va + (u64)len, 0) != 0)             { spoor_clunk(c); return -1; }   // trailing NUL

    spoor_clunk(c);
    return (s64)len;
}

// =============================================================================
// LS-K (ARCH §22.6): identity reads + clock_gettime. The three identity calls
// return the calling Proc's durable fields (no args, no memory write, no
// capability); the field values are < 2^32 so the s64 return is never negative
// (no error-aliasing). SYS_CLOCK_GETTIME fills a t_timespec for MONOTONIC
// (CNTVCT) or the boot-anchored REALTIME wall clock. All four are NON-static so
// the kernel tests call them directly. current_thread() is always valid in a
// syscall frame -- the !t / !p guards are defense-in-depth.
// =============================================================================

s64 sys_getpid_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return (s64)p->pid;
}

s64 sys_getuid_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return (s64)(u64)p->principal_id;
}

s64 sys_getgid_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return (s64)(u64)p->primary_gid;
}

s64 sys_clock_gettime_handler(u64 clk_id, u64 ts_va, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    // Validate the clock id FIRST -- a bad id never touches the buffer.
    u64 ns;
    switch (clk_id) {
    case T_CLOCK_REALTIME:  ns = timer_realtime_ns(); break;
    case T_CLOCK_MONOTONIC: ns = timer_now_ns();      break;
    default:                return -T_E_INVAL;
    }
    if (!sys_validate_user_buf(ts_va, sizeof(struct t_timespec)))
        return -T_E_FAULT;
    // uaccess_store_u32 requires a 4-byte-aligned target (an unaligned STR
    // alignment-faults, which the uaccess fixup table does NOT catch -> kernel
    // extinction the moment SCTLR_EL1.A is set). All four stores sit at
    // ts_va + {0,4,8,12}, so a 4-byte-aligned ts_va aligns every store. Reject
    // a misaligned ts_va up front (a conformant struct t_timespec is 8-aligned).
    if (ts_va & 0x3u)
        return -T_E_FAULT;

    u64 sec  = ns / 1000000000ull;
    u32 nsec = (u32)(ns % 1000000000ull);
    // struct t_timespec { s64 tv_sec @0; s64 tv_nsec @8 }. aarch64 is
    // little-endian, so each i64 is [low u32, high u32]. tv_sec fits ~33 bits
    // (epoch ~1.7e9 s) so its high word is small but nonzero; tv_nsec < 1e9
    // fits a u32 (high word 0). Stored via the audited uaccess_store_u32 (no
    // uaccess_store_u64 exists); any store fault -> -EFAULT, nothing else
    // touched.
    if (uaccess_store_u32(ts_va + 0,  (u32)(sec & 0xFFFFFFFFu)) != 0) return -T_E_FAULT;
    if (uaccess_store_u32(ts_va + 4,  (u32)(sec >> 32))         != 0) return -T_E_FAULT;
    if (uaccess_store_u32(ts_va + 8,  nsec)                     != 0) return -T_E_FAULT;
    if (uaccess_store_u32(ts_va + 12, 0u)                       != 0) return -T_E_FAULT;
    return 0;
}

// SYS_CLOCK_SETTIME(clk_id, timespec_va) -- step CLOCK_REALTIME (net-7a). The
// SNTP client's clock-step path. Re-anchors the single wall-clock offset;
// MONOTONIC is untouched. CAP_HOSTOWNER-gated (a clock step is system-global, so
// it is the host owner's authority, never an identity's -- I-22). The clk_id +
// the cap are validated BEFORE any buffer read.
s64 sys_clock_settime_handler(u64 clk_id, u64 ts_va, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    // Only CLOCK_REALTIME is settable. MONOTONIC is the boot-counter timebase and
    // cannot be stepped (the Linux/POSIX rule); an unknown id is also EINVAL.
    if (clk_id != T_CLOCK_REALTIME)
        return -T_E_INVAL;

    struct Thread *t = current_thread();
    if (!t) return -1;
    struct Proc *p = t->proc;
    if (!p) return -1;
    // p->caps has a cross-thread writer since A-4a -> an atomic acquire load.
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HOSTOWNER) == 0)
        return -T_E_ACCES;

    if (!sys_validate_user_buf(ts_va, sizeof(struct t_timespec)))
        return -T_E_FAULT;
    // uaccess_load_u32 reads sit at ts_va + {0,4,8,12}; require 4-byte alignment
    // so each LDR is aligned (an unaligned LDR alignment-faults once SCTLR_EL1.A
    // is set, which the uaccess fixup table does NOT catch -> extinction). A
    // conformant struct t_timespec is 8-aligned. Mirrors SYS_CLOCK_GETTIME.
    if (ts_va & 0x3u)
        return -T_E_FAULT;

    // struct t_timespec { s64 tv_sec @0; s64 tv_nsec @8 }. aarch64 is
    // little-endian: each i64 is [low u32, high u32]. No uaccess_load_u64 exists.
    u32 sec_lo, sec_hi, nsec_lo, nsec_hi;
    if (uaccess_load_u32(ts_va + 0,  &sec_lo)  != 0) return -T_E_FAULT;
    if (uaccess_load_u32(ts_va + 4,  &sec_hi)  != 0) return -T_E_FAULT;
    if (uaccess_load_u32(ts_va + 8,  &nsec_lo) != 0) return -T_E_FAULT;
    if (uaccess_load_u32(ts_va + 12, &nsec_hi) != 0) return -T_E_FAULT;
    s64 tv_sec  = (s64)(((u64)sec_hi  << 32) | sec_lo);
    s64 tv_nsec = (s64)(((u64)nsec_hi << 32) | nsec_lo);

    // POSIX clock_settime(CLOCK_REALTIME): reject a negative time and an
    // out-of-range nanosecond. Bound tv_sec so tv_sec*1e9 + tv_nsec cannot
    // overflow u64 (CLOCK_SETTIME_SEC_MAX*1e9 ~= 1e19 < 1.8e19; year ~2286, far
    // past any realistic step). A small-but-valid epoch is accepted -- the timer
    // publish fail-soft-floors a degenerate epoch_ns < mono to offset 0.
    if (tv_sec < 0 || tv_nsec < 0 || tv_nsec >= 1000000000)
        return -T_E_INVAL;
    if ((u64)tv_sec > 10000000000ull)   // ~year 2286; guards the multiply below
        return -T_E_INVAL;

    u64 epoch_ns = (u64)tv_sec * 1000000000ull + (u64)tv_nsec;
    timer_reset_wallclock_anchor_ns(epoch_ns);
    return 0;
}

static s64 sys_open_handler(u64 start_fd_raw, u64 path_va,
                            u64 path_len_raw, u64 omode_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if (path_len_raw == 0)                           return -1;
    if (path_len_raw > SYS_OPEN_PATH_MAX)            return -1;
    if (!sys_validate_user_buf(path_va, path_len_raw)) return -1;
    if (omode_raw & ~(u64)SYS_WALK_OPEN_OMODE_VALID) return -1;

    // Resolve the base Spoor (BORROWED — stalk never refs/clunks it). FROM_ROOT
    // uses the Territory's pivoted root_spoor; otherwise a KOBJ_SPOOR handle
    // gated on RIGHT_READ (the capability axis; stalk's per-component perm_check
    // is the orthogonal identity axis).
    struct Spoor *start;
    if (start_fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
        if (!p->territory)                           return -1;
        // RW-4 SA-F1: atomic read+ref under ns_lock (the prior read-then-ref
        // raced a concurrent pivot_root that frees the old root mid-window).
        start = territory_root_ref(p->territory);
        if (!start)                                  return -1;
    } else {
        start = sys_lookup_spoor(p, (hidx_t)start_fd_raw, RIGHT_READ);   // ref-held
        if (!start)                                  return -1;
    }

    // Copy the path into kernel scratch + reject embedded NUL (truncation /
    // wire-leak vector). '/' is ALLOWED here (multi-component) — stalk
    // tokenizes it. The scratch is one byte over so the NUL terminator below
    // is always writable even at the max length.
    char path_scratch[SYS_OPEN_PATH_MAX + 1];
    for (u64 i = 0; i < path_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(path_va + i, &b) != 0)   { spoor_clunk(start); return -1; }
        if (b == '\0')                               { spoor_clunk(start); return -1; }
        path_scratch[i] = (char)b;
    }
    path_scratch[path_len_raw] = '\0';

    // LS-4: a RELATIVE path with the FROM_ROOT sentinel resolves against the
    // Territory cwd (dot) -- POSIX openat(AT_FDCWD, ...). Join + lexically clean
    // dot + path into an absolute path, then resolve from root (start is already
    // root_spoor). An explicit start-fd (a dirfd) or an absolute path is
    // unchanged. stalk still re-clamps ".." at root_spoor, so the join cannot
    // escape containment (I-28 preserved; no new mechanism).
    char joined[SYS_OPEN_PATH_MAX + 1];
    const char *rpath = path_scratch;
    u64 rlen = path_len_raw;
    if (start_fd_raw == SYS_WALK_OPEN_FROM_ROOT && path_scratch[0] != '/') {
        int jl = territory_resolve_cwd(p->territory, path_scratch, path_len_raw,
                                       joined, sizeof(joined));
        if (jl < 0) { spoor_clunk(start); return -1; }
        rpath = joined;
        rlen  = (u64)jl;
    }

    int amode = (omode_raw & SYS_WALK_OPEN_OPATH) ? STALK_WALK : STALK_OPEN;
    struct Spoor *quarry = stalk(p, start, rpath, rlen,
                                 amode, (u32)omode_raw);
    // #844: start (BORROWED by stalk -- it never refs/clunks it) is done now;
    // release the borrow. quarry owns its own ref from stalk.
    spoor_clunk(start);
    if (!quarry)                                     return -1;

    // Handle rights, identical policy to sys_walk_open_handler: an O_PATH
    // (walk-only) handle is born R|W with NO RIGHT_TRANSFER (a navigation /
    // capability base, A-1.7/F5); a normally-opened handle derives its rights
    // from omode (A-3b) so the capability axis cannot exceed the access stalk's
    // final perm_check validated, plus RIGHT_TRANSFER. The quarry owns its ref
    // (from stalk); handle_alloc takes it; on a full table we clunk it.
    rights_t r;
    if (omode_raw & SYS_WALK_OPEN_OPATH) {
        r = RIGHT_READ | RIGHT_WRITE;
        quarry->flag |= CWALKONLY;   // #81: a navigation handle -- sys_read/write/readdir reject it
    } else {
        r = rights_for_omode((u32)omode_raw) | RIGHT_TRANSFER;
    }
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, r, quarry);
    if (fd < 0) {
        spoor_clunk(quarry);
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
        // RW-4 SA-F1: atomic read+ref under ns_lock (closes the read-then-ref
        // UAF window vs a concurrent pivot_root).
        src = territory_root_ref(p->territory);
        if (!src)                                     return -1;
    } else {
        src = sys_lookup_spoor(p, (hidx_t)parent_fd_raw, RIGHT_WRITE);   // ref-held
        if (!src)                                     return -1;
    }
    if (!src->dev || !src->dev->walk || !src->dev->create) { spoor_clunk(src); return -1; }

    // A-2d: write + search (W|X) permission on the parent directory before
    // creating in it. Gated on perm_enforced (dev9p deferred to A-3). devramfs
    // is read-only (its .create stub returns NULL) so this is effectively dead
    // for devramfs, but correct: a non-system principal lacks other-w on a 0755
    // dir and is denied here before the create attempt. fail-closed on no stat.
    if (src->dev->perm_enforced) {
        struct t_stat parent_st;
        if (spoor_stat_native(src, &parent_st) != 0)          { spoor_clunk(src); return -1; }
        if (perm_check(p, &parent_st, PERM_W | PERM_X) != 0)  { spoor_clunk(src); return -1; }
    }

    // Copy + validate the component name (same strict shape as SYS_WALK_OPEN:
    // reject '/' '\0' "." ".."; NUL-terminate for dev9p's strlen scan).
    char name_scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(name_va + i, &b) != 0)    { spoor_clunk(src); return -1; }
        if (b == '/' || b == '\0')                    { spoor_clunk(src); return -1; }
        name_scratch[i] = (char)b;
    }
    if (name_len_raw == 1 && name_scratch[0] == '.')  { spoor_clunk(src); return -1; }
    if (name_len_raw == 2 && name_scratch[0] == '.' &&
                              name_scratch[1] == '.') { spoor_clunk(src); return -1; }
    name_scratch[name_len_raw] = '\0';

    // stalk-3b (STALK-DESIGN.md §5.3 / D2): a CREATE against a /srv directory
    // (a devsrv root Spoor: dc='s', aux = a SrvRegistry) is a service POST, not
    // a file create. It mints a KObj_Srv LISTENER -- a different handle kind
    // than the KOBJ_SPOOR the generic create path installs over the returned
    // Spoor -- so it cannot ride that path; branch here and return the listener
    // hidx directly. perm selects the transport: DMSRVBYTE -> byte-mode, else
    // 9P-mode; no other perm bit is meaningful for a service post.
    if (src->dc == 's' && src->aux &&
        *(const u64 *)src->aux == SRV_REGISTRY_MAGIC) {
        if (perm & ~SYS_WALK_CREATE_DMSRVBYTE)          { spoor_clunk(src); return -1; }
        enum srv_mode mode = (perm & SYS_WALK_CREATE_DMSRVBYTE)
                                 ? SRV_MODE_BYTE : SRV_MODE_9P;
        // #844: devsrv_post_listener mints a registry-lifetime KObj_Srv (not
        // tied to src); release the src borrow after it returns.
        s64 lh = (s64)devsrv_post_listener(p, src, name_scratch,
                                           (size_t)name_len_raw, mode);
        spoor_clunk(src);
        return lh;
    }

    // DMSRVBYTE is meaningful ONLY for the /srv service post above. On a
    // regular create it must not reach a Dev's create perm (e.g. a dev9p
    // Tlcreate), where the high bit would corrupt the wire perm -- reject it.
    if (perm & SYS_WALK_CREATE_DMSRVBYTE)                 { spoor_clunk(src); return -1; }

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
    if (!nc)                                          { spoor_clunk(src); return -1; }

    struct Walkqid *w = src->dev->walk(src, nc, NULL, 0);
    // #844: src's last use is the clone-walk above; release the borrow now.
    // nc carries its own ref through create + the rest of the handler.
    spoor_clunk(src);
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
    // #66 audit F5: the generic create path installs `nc` (handle_alloc below), so
    // it relies on create returning nc opened in place. dev9p does; devramfs's
    // create stub returns NULL (handled above); the /srv post branched earlier.
    // A FUTURE Dev whose create returns a DIFFERENT Spoor (the devsrv-open
    // precedent shows the shape exists) would leak `opened` + install the wrong
    // node -- the exact open-side asymmetry that produced #957-F1. Reject it (the
    // generic path cannot correctly adopt a non-nc create result). Unreachable at
    // v1.0; defensive symmetry with the open arm.
    if (opened != nc) {
        spoor_clunk(opened);
        spoor_clunk(nc);
        return -1;
    }

    // #66: append the created name to nc's namespace name. nc SHARES src's Path
    // (the clone-walk does not extend it -- nname==0); spoor_path_extend reads
    // that shared (parent) Path and installs the extended one. Non-load-bearing
    // (I-33). create returns nc opened in place (opened == nc), the Spoor
    // installed below.
    spoor_path_extend(nc, name_scratch, name_len_raw);

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

    // #844: c is REF-HELD (borrow); spoor_clunk on every exit (fsync may block).
    struct Spoor *c = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_WRITE);
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->fsync)                 { spoor_clunk(c); return -1; }

    // Normalize datasync to 0/1 (any non-zero is "data only").
    u32 datasync = (datasync_raw != 0) ? 1u : 0u;
    int rc = c->dev->fsync(c, datasync);
    spoor_clunk(c);
    return rc == 0 ? 0 : -1;
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

    // #844: c is REF-HELD (borrow); spoor_clunk on every exit (readdir blocks).
    struct Spoor *c = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_READ);
    if (!c)                                          return -1;
    // #81: a T_OPATH navigation handle is NOT a byte-I/O channel -- reject readdir
    // too (listing a dir's entries is content the perm_check-exempt O_PATH open
    // would otherwise leak for a non-readable dir). IDENTITY-DESIGN 9.4 #81.
    if (c->flag & CWALKONLY)                       { spoor_clunk(c); return -1; }
    if (!c->dev || !c->dev->readdir)               { spoor_clunk(c); return -1; }

    u8 scratch[SYS_RW_MAX];
    u64 in_cookie = (u64)c->offset;   // the opaque resume cookie we ask to resume FROM
    long got = c->dev->readdir(c, scratch, (long)buf_len_raw, c->offset);
    if (got < 0)                                   { spoor_clunk(c); return -1; }
    if (got == 0)                                  { spoor_clunk(c); return 0; }   // EOD

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
    if (!advanced)                                 { spoor_clunk(c); return -1; }   // malformed run

    // Defense-in-depth (#955): a non-empty run whose last cookie == the cookie
    // we resumed from means the cursor did not advance -- a paginating reader
    // would re-fetch this same batch forever. Those entries were already
    // delivered by the call that produced this cursor, so report EOD instead of
    // re-delivering + spinning. The primary #955 fix (opaque-u64 cookie
    // round-trip in dev9p_readdir) keeps a correct server from reaching here;
    // this bounds a buggy/hostile one (the v1.x untrusted-server posture).
    //
    // CONTRACT (load-bearing): this never truncates a real listing ONLY because
    // a correct server's per-entry cookie is STRICTLY MONOTONIC across a resumed
    // enumeration -- the first entry of any non-empty resume batch has a cookie
    // > in_cookie. Stratum satisfies this (unique per-entry cookies, sorted
    // ascending, strict-`<` resume filter); devramfs satisfies it (1-based
    // ordinals). The `in_cookie != 0` carve-out keeps the FIRST call (offset 0,
    // never-yet-advanced) always delivering -- no on-wire cookie is ever 0
    // (Stratum + devramfs both start at 1). A server that re-emits the resume
    // entry with an EQUAL cookie would have its listing truncated here -- that
    // is the untrusted-server seam, not a correct-server case.
    if (last_cookie == in_cookie && in_cookie != 0) { spoor_clunk(c); return 0; }

    // Copy the dirent bytes to user-VA FIRST, THEN advance the Spoor offset
    // (F3 audit). If a uaccess store faults, we return -1 with the offset
    // UNCHANGED, so the caller's retry re-fetches the same run rather than
    // silently skipping the entries it never received.
    for (long i = 0; i < got; i++) {
        if (uaccess_store_u8(buf_va + (u64)i, scratch[i]) != 0) { spoor_clunk(c); return -1; }
    }
    c->offset = (s64)last_cookie;
    spoor_clunk(c);
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
// #844: returns a REF-HELD dir Spoor (caller MUST spoor_clunk). The handle
// branch gets sys_lookup_spoor's transferred ref; the FROM_ROOT branch takes an
// explicit spoor_ref on the Territory root so the caller's clunk is uniform
// (and the root survives a concurrent pivot_root). NULL on failure (no ref).
static struct Spoor *sys_resolve_dir_wr(struct Proc *p, u64 fd_raw) {
    if (fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
        // RW-4 SA-F1: atomic read+ref under ns_lock (territory_root_ref handles a
        // NULL Territory) -- the prior read-then-ref raced a concurrent pivot_root.
        return territory_root_ref(p->territory);
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

    // #844: od + nd are REF-HELD borrows; spoor_clunk BOTH on every exit. od==nd
    // (same dir fd / both FROM_ROOT) means each resolve took a ref -> two clunks
    // balance. Held across the (possibly blocking) stat + rename 9P ops.
    struct Spoor *od = sys_resolve_dir_wr(p, olddir_fd_raw);
    if (!od)                                          return -1;
    struct Spoor *nd = sys_resolve_dir_wr(p, newdir_fd_raw);
    if (!nd)                                        { spoor_clunk(od); return -1; }
    if (!od->dev || !od->dev->rename)              { spoor_clunk(od); spoor_clunk(nd); return -1; }
    // Two-cursor + cross-Dev invariant: a 9P renameat is within ONE server, so
    // both directories MUST be on the same Dev (dev9p_rename adds the same-
    // session guard). Rejected here before any Dev op.
    if (od->dev != nd->dev)                        { spoor_clunk(od); spoor_clunk(nd); return -1; }

    // A-3b (closes A-2d audit F2): rwx enforcement on dir mutation. POSIX
    // rename needs write + search (W|X) on BOTH parent dirs. Gated on the
    // Dev's perm_enforced (devramfs leaves .rename NULL; dev9p enforces from
    // A-3b). od->dev == nd->dev here, so one flag governs both.
    if (od->dev->perm_enforced) {
        struct t_stat ost, nst;
        if (spoor_stat_native(od, &ost) != 0)             { spoor_clunk(od); spoor_clunk(nd); return -1; }
        if (perm_check(p, &ost, PERM_W | PERM_X) != 0)    { spoor_clunk(od); spoor_clunk(nd); return -1; }
        if (spoor_stat_native(nd, &nst) != 0)             { spoor_clunk(od); spoor_clunk(nd); return -1; }
        if (perm_check(p, &nst, PERM_W | PERM_X) != 0)    { spoor_clunk(od); spoor_clunk(nd); return -1; }
    }

    int rc = od->dev->rename(od, old_scratch, nd, new_scratch);
    spoor_clunk(od);
    spoor_clunk(nd);
    return rc == 0 ? 0 : -1;
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

    // #844: c is a REF-HELD borrow; spoor_clunk on every exit (held across the
    // possibly-blocking stat + unlink 9P ops).
    struct Spoor *c = sys_resolve_dir_wr(p, parent_fd_raw);
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->unlink)                { spoor_clunk(c); return -1; }

    // A-3b (closes A-2d audit F2): W|X on the parent dir to remove an entry
    // (POSIX). Gated on perm_enforced (dev9p enforces from A-3b; devramfs
    // leaves .unlink NULL).
    if (c->dev->perm_enforced) {
        struct t_stat cst;
        if (spoor_stat_native(c, &cst) != 0)              { spoor_clunk(c); return -1; }
        if (perm_check(p, &cst, PERM_W | PERM_X) != 0)    { spoor_clunk(c); return -1; }
    }

    int rc = c->dev->unlink(c, scratch, (u32)flags_raw);
    spoor_clunk(c);
    return rc == 0 ? 0 : -1;
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
    // #844: c is REF-HELD (KOBJ_SPOOR + RIGHT_WRITE); spoor_clunk on every exit
    // -- the ref keeps c alive across the (possibly blocking) stat/setattr.
    struct Spoor *c = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_WRITE);
    if (!c)                                          return -1;

    // A-2d: the ownership-change policy (IDENTITY-DESIGN.md 3.7.1 + perm.c).
    // Gated on perm_enforced (dev9p deferred to A-3; devramfs .wstat_native is
    // NULL so SYS_WSTAT on it returns -1 below regardless). Reads the file's
    // CURRENT owner, then applies the policy.
    if (c->dev && c->dev->perm_enforced) {
        struct t_stat cur;
        if (spoor_stat_native(c, &cur) != 0)              { spoor_clunk(c); return -1; }
        if (perm_wstat_check(p, cur.uid, valid, gid) != 0){ spoor_clunk(c); return -1; }
    }
    int rc = spoor_wstat_native(c, valid, mode, uid, gid);
    spoor_clunk(c);
    return rc == 0 ? 0 : -1;
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

    // territory_chroot handles: idempotent same-pointer (returns 0 without ref
    // bump), prior-root displacement (spoor_clunk the old), spoor_ref of the
    // new source. #844: source is REF-HELD (a borrow); territory_chroot takes
    // its own ref, so release the borrow after (also covers the idempotent
    // same-pointer path, where chroot took no new ref).
    int rc = territory_chroot(p->territory, source);
    spoor_clunk(source);
    return rc == 0 ? 0 : -1;
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
                                    u64 arg_va, u64 tls_va, u64 ptid_va) {
    struct Thread *t = current_thread();
    if (!t)                                          return -T_E_INVAL;
    struct Proc *p = t->proc;
    if (!p)                                          return -T_E_INVAL;
    if (p->magic != PROC_MAGIC)                      return -T_E_INVAL;
    if (p == kproc())                                return -T_E_INVAL;
    if (p->pgtable_root == 0)                        return -T_E_INVAL;

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
    if (entry_va == 0)                               return -T_E_INVAL;
    if (entry_va & 0x3u)                              return -T_E_INVAL;
    if (entry_va >= UACCESS_USER_VA_TOP)              return -T_E_INVAL;

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
    if (sp_va == 0)                                   return -T_E_INVAL;
    if ((sp_va & 0xFu) != 0)                          return -T_E_INVAL;
    if (sp_va >= UACCESS_USER_VA_TOP)                 return -T_E_INVAL;

    // tls_va: 0 is permitted (no TLS yet — the entry can set it
    // afterward via msr tpidr_el0). Non-zero must be in user VA. No
    // alignment requirement — TLS layout is libc-defined.
    if (tls_va != 0 && tls_va >= UACCESS_USER_VA_TOP) return -T_E_INVAL;

    // ptid_va (#112, CLONE_PARENT_SETTID): 0 opts out of the publish.
    // Non-zero must be 4-byte aligned + within user VA — the same gate as
    // SYS_SET_TID_ADDRESS's tidptr, because the publish below uses
    // uaccess_store_u32 and a misaligned STR faults. A bad ptid is a clean
    // -EINVAL at the gate, before any Thread is created.
    if (ptid_va != 0) {
        if ((ptid_va & 0x3u) != 0)                   return -T_E_INVAL;
        if (ptid_va >= UACCESS_USER_VA_TOP)          return -T_E_INVAL;
    }

    // #65 (I-32): the per-Proc thread cap. A non-TCB Proc at PROC_THREAD_MAX is
    // refused -EAGAIN (the POSIX RLIMIT_NPROC convention) before the kstack
    // alloc -- bounding a thread bomb (each thread pins unswappable kernel
    // kstack). kproc is already rejected above; the SYSTEM boot/service chain is
    // exempt. A bounded TOCTOU overshoot (<= ncpus-1) is acceptable for a floor.
    if (!proc_thread_cap_ok(p))                      return -T_E_AGAIN;

    struct Thread *nt = thread_create_user(p, entry_va, sp_va, arg_va, tls_va);
    if (!nt)                                         return -T_E_NOMEM;

    // CLONE_PARENT_SETTID (#112): publish the new tid into the parent-
    // supplied user word BEFORE the child is made runnable. Parent and
    // child share the address space (one pgtable_root), so this single
    // store serves both; ready()'s run-queue lock release is the
    // happens-before edge that carries it to whichever CPU first picks up
    // the child, so the child can never observe a 0 tid -- closing the
    // #111 window at its ROOT (the child no longer depends on a racing
    // parent-side store), not merely from the child side. nt->tid is read
    // HERE, before ready(), so the load cannot race the child's first
    // dispatch + eventual thread_free.
    //
    // Best-effort BY CONTRACT (not merely "in practice"): ptid_va is a
    // NATIVE surface (t_thread_spawn / spawn_raw) any EL0 program can call
    // with an arbitrary align+bound-legal address. The alignment gate above
    // is load-bearing -- an UNaligned STR would extinct, since the EL1 fixup
    // table catches only translation/permission/access-flag faults, not
    // alignment -- but a mapped-RO or unmapped in-bound aligned target
    // routes through the demand-page write path, returns -1, and is
    // SWALLOWED: NOT a spawn failure and NOT an extinction, because (a) the
    // tid is returned authoritatively in x0 regardless, and (b) it matches
    // the exit-time clear_child_tid store's best-effort discipline
    // (kernel/proc.c::thread_clear_child_tid_handoff). Tolerating it also
    // keeps the new Thread off any rollback path (no thread_free of a
    // never-readied Thread on a transient uaccess fault). The pouch consumer
    // passes &new->tid -- always writable -- so the swallow is never taken
    // there; it exists only so a buggy/hostile native ptid is neither a
    // spawn failure nor an extinction.
    if (ptid_va != 0)
        (void)uaccess_store_u32(ptid_va, (u32)nt->tid);

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
    // thread_exit_self is __noreturn. Match the EXITS/EXIT_GROUP siblings'
    // extinction backstop (RW-3 R2-F2): if the "noreturn" ever returns, fail
    // loud here rather than UB-falling-through into the next dispatch case.
    extinction("sys_thread_exit returned");
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
    // LS-5 (P2 default disposition, ARCH 8.8.2): this Proc has declared it
    // consumes its own notes via the fd-read path -- mark it self-managing so
    // the EL0-return-tail uncaught-`interrupt` default-terminate EXEMPTS it
    // (a self-managing Proc reads + acts on its interrupt; only a non-self-
    // managing handler-less Proc auto-terminates). One-way; set only after the
    // fd exists (the declaration is "I successfully obtained a notes fd").
    // LS-5c: routed through notes_mark_self_managing, which also clears the
    // terminate latch under q->lock (the disposition just changed to consume).
    notes_mark_self_managing(p);
    return (s64)fd;
}

// SYS_NOTIFY(handler_va) — register/clear the async note handler.
// F9 audit close: release-store so a multi-thread Proc's other Thread
// observing handler_va in notes_deliver_at_el0_return sees a coherent
// value (paired with the acquire-load in notes.c). LS-5c: the store runs
// inside notes_set_handler under q->lock so it serializes against
// notes_post's check-handler-then-arm of the terminate latch (registering
// a handler un-arms it).
static s64 sys_notify_handler(u64 handler_va) {
    struct Thread *t = current_thread();
    if (!t || !t->proc) return -1;
    struct Proc *p = t->proc;

    if (handler_va != 0) {
        if (handler_va >= UACCESS_USER_VA_TOP) return -1;
        if (handler_va & 0x3) return -1;     // aarch64 instructions are 4-aligned
    }
    notes_set_handler(p, handler_va);
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
        // NDFLT -- default action: exits(name). For the v1.0 supported set
        // every uncaught default is process termination. Post-#811 (ARCH
        // 8.8.1) exits() on a Proc with live peers CASCADES via
        // proc_group_terminate (it no longer extincts), so a multi-thread
        // Proc's uncaught default-terminate signal terminates the WHOLE Proc
        // -- the POSIX default action. RW-8 R5-F1: the prior live-peers
        // refusal (-1) predated #809/#811 and, with pouch's always-installed
        // handler bypassing the LS-5 kernel default-terminate, silently
        // swallowed SIGINT/SIGTERM in multi-thread pouch daemons (NDFLT
        // refused -> bootstrap NCONT-resumes -> the signal evaporates). The
        // cascade is the same #811 path a multi-thread exits() already takes.
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
    // LS-5c (P3-terminate): if the post armed the terminate latch (an
    // `interrupt` to a handler-less non-self-managing target -- the shell's
    // Ctrl-C forward to a blocked foreground child is the canonical case),
    // wake the target's blocked threads so the LS-5b terminate fires at
    // their EL0-return tails. Internally gated on the latch; the walk's
    // g_proc_table_lock contract is satisfied (proc_for_each holds it).
    if (rc == 0) proc_interrupt_terminate_wake(target);
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
        // LS-5c (P3-terminate): a self-post of `interrupt` in a multi-thread
        // Proc may arm the terminate latch while a PEER thread is blocked in
        // a rendez sleep -- wake the peers so they unwind to their tails (the
        // posting thread itself is running and reaches its own tail at this
        // syscall's return). The wake walks p->threads, so it needs
        // g_proc_table_lock (the #811 contract); take it only when the latch
        // armed (the read is a benign pre-check -- the wake re-validates
        // under its own internal gate, and `p` is self, immune to reap here).
        if (rc == 0 && proc_intr_terminate_pending(p)) {
            irq_state_t ws = proc_table_lock_acquire();
            proc_interrupt_terminate_wake(p);
            proc_table_lock_release(ws);
        }
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

    // #65 (I-32): charge this Proc's anon-page floor BEFORE committing the
    // eager allocation. Under vma_lock, so the check+charge is atomic against a
    // sibling attach (the cap is exact). A non-TCB Proc over PROC_PAGE_MAX is
    // refused with -ENOMEM here -- it never reaches the allocator. Uncharged on
    // every failure path below + on SYS_BURROW_DETACH.
    u32 npages = (u32)(length / PAGE_SIZE);
    if (!proc_page_charge(p, npages)) {
        spin_unlock(&p->vma_lock);
        return -T_E_NOMEM;
    }

    // burrow_create_anon: handle_count = 1 (the construction reference),
    // mapping_count = 0; pages allocated eagerly (power-of-2 rounded).
    struct Burrow *b = burrow_create_anon(length);
    if (!b) {
        proc_page_uncharge(p, npages);
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
        proc_page_uncharge(p, npages);
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
    // #65 (I-32): uncharge the anon-page floor ONLY on a successful unmap (rc==0
    // means the pages were freed); the same rounded length that attach charged.
    // Under vma_lock, so it pairs exactly with the charge.
    if (rc == 0)
        proc_page_uncharge(p, (u32)(length / PAGE_SIZE));
    spin_unlock(&p->vma_lock);

    return (s64)rc;
}

static s64 sys_burrow_detach_handler(u64 vaddr_raw, u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    return sys_burrow_detach_for_proc(t->proc, vaddr_raw, length_raw);
}

// =============================================================================
// Loom -- the io_uring-inverted 9P ring transport (Loom-2a). kernel/loom.c owns
// the KObj_Loom + the ring substrate; these inners wire a ring into a Proc's
// address space + handle table (the sys_burrow_attach_for_proc factoring). The
// SQE dispatch + the submit-time pin + the CQE post (SYS_LOOM_ENTER) are Loom-3.
// =============================================================================

int sys_loom_setup_for_proc(struct Proc *p, u32 entries, u32 flags,
                            struct loom_params *out, hidx_t *out_fd) {
    if (!p || !out || !out_fd)                       return -1;
    // Loom-4c accepts LOOM_SETUP_SQPOLL; LOOM_SETUP_CQSIZE (caller-chosen cq)
    // is still reserved until its sub-chunk lands, so reject it + any unknown bit.
    if (flags & ~LOOM_SETUP_SQPOLL)                  return -1;
    if (entries == 0 || entries > LOOM_MAX_ENTRIES)  return -1;
    if ((entries & (entries - 1u)) != 0)             return -1;  // power of two

    // io_uring default: CQ twice the SQ. `entries` is a power of two <=
    // LOOM_MAX_ENTRIES, so cq_entries is a power of two <= 2*LOOM_MAX_ENTRIES.
    u32 cq_entries = entries * 2u;

    struct Loom *l = loom_create(entries, cq_entries);
    if (!l)                                          return -1;

    // #65 (I-32 / audit F1): the ring is anonymous pages mapped into the Proc's
    // address space -- the SAME memory-bomb class SYS_BURROW_ATTACH is capped
    // for. SYS_LOOM_SETUP is EL0-reachable + repeatable (close reuses the handle
    // slot while mapping_count keeps the ring VMA alive), so WITHOUT this charge
    // a non-TCB Proc accumulates uncharged anon up to the physical cliff,
    // defeating the per-Proc page cap. Charge ring_size's pages here (under the
    // same vma_lock as the map -> exact); relief comes from SYS_BURROW_DETACH on
    // the ring VA (the ring is a normal VMA in the burrow window, so the existing
    // detach uncharge fires) or vma_drain at exit. The close-without-detach
    // accumulation therefore hits PROC_PAGE_MAX instead of RAM.
    u32 ring_pages = (u32)(l->ring_size / PAGE_SIZE);

    // Map the ring RW into the burrow-attach window. burrow_map takes its OWN
    // mapping_count ref (vma_alloc -> burrow_acquire_mapping); the Loom keeps
    // its handle_count ref, so the ring stays alive while EITHER side holds it
    // (the #847 dual-refcount). vma_find_gap + burrow_map under one vma_lock so
    // a sibling thread cannot claim the same gap.
    spin_lock(&p->vma_lock);
    u64 vaddr;
    if (vma_find_gap(p, (size_t)l->ring_size, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &vaddr) != 0) {
        spin_unlock(&p->vma_lock);
        loom_unref(l);
        return -1;
    }
    if (!proc_page_charge(p, ring_pages)) {
        spin_unlock(&p->vma_lock);
        loom_unref(l);
        return -1;                                   // over the per-Proc page cap
    }
    if (burrow_map(p, l->ring, vaddr, (size_t)l->ring_size, VMA_PROT_RW) != 0) {
        proc_page_uncharge(p, ring_pages);
        spin_unlock(&p->vma_lock);
        loom_unref(l);
        return -1;
    }
    spin_unlock(&p->vma_lock);

    // Loom-4c: spawn the SQPOLL poll-thread BEFORE installing the handle, so
    // every failure path below reclaims it through loom_unref -> loom_free's
    // join (loom_free sees l->sqpoll set and stops + reaps the kthread). The
    // earlier failure paths (above) ran with l->sqpoll == NULL, so loom_free
    // skipped the join there. The kthread immediately parks (no SQEs yet).
    if (flags & LOOM_SETUP_SQPOLL) {
        if (loom_start_sqpoll(l) != 0) {
            spin_lock(&p->vma_lock);
            (void)burrow_unmap(p, vaddr, (size_t)l->ring_size);
            proc_page_uncharge(p, ring_pages);       // #65: the ring VMA freed
            spin_unlock(&p->vma_lock);
            loom_unref(l);
            return -1;
        }
    }

    // Install the handle. It ADOPTS the Loom's creation refcount (=1): a later
    // handle_close -> handle_release_obj(KOBJ_LOOM) -> loom_unref drops it. On
    // alloc failure, unmap + loom_unref fully reclaims (handle_count to 0 via
    // loom_free's burrow_unref -- which first JOINS any spawned SQPOLL kthread --
    // mapping_count to 0 via burrow_unmap).
    hidx_t fd = handle_alloc(p, KOBJ_LOOM, RIGHT_READ | RIGHT_WRITE, l);
    if (fd < 0) {
        spin_lock(&p->vma_lock);
        (void)burrow_unmap(p, vaddr, (size_t)l->ring_size);
        proc_page_uncharge(p, ring_pages);           // #65: the ring VMA freed
        spin_unlock(&p->vma_lock);
        loom_unref(l);
        return -1;
    }

    out->flags         = flags;   // echo the granted setup flags (LOOM_SETUP_SQPOLL)
    out->sq_entries    = l->sq_entries;
    out->cq_entries    = l->cq_entries;
    out->ring_size     = l->ring_size;
    out->ring_va       = vaddr;
    out->hdr_off       = l->hdr_off;
    out->sq_array_off  = l->sq_array_off;
    out->sqe_off       = l->sqe_off;
    out->cqe_off       = l->cqe_off;
    out->sq_array_size = l->sq_array_size;
    out->sqe_size      = l->sqe_size;
    out->cqe_size      = l->cqe_size;
    out->_resv0        = 0;
    out->_resv1[0] = out->_resv1[1] = out->_resv1[2] = out->_resv1[3] = 0;
    *out_fd = fd;
    return 0;
}

int sys_loom_register_for_proc(struct Proc *p, hidx_t loom_fd, u32 op,
                               const hidx_t *fds, u32 n) {
    if (!p)                            return -1;
    if (op != LOOM_REGISTER_HANDLES)   return -1;   // BUFFERS reserved (Loom-6)
    if (n > LOOM_MAX_REG_HANDLES)      return -1;
    if (n > 0 && !fds)                 return -1;

    struct Handle lh;
    if (handle_get(p, loom_fd, &lh) != 0)  return -1;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -1; }
    struct Loom *l = (struct Loom *)lh.obj;

    // Resolve each fd -> KOBJ_SPOOR, taking the table's OWN ref + snapshotting
    // rights. handle_get holds a ref under the table lock; we spoor_ref a
    // SECOND, independent ref for the ring then handle_put the get's ref -- so
    // the ring's ref is decoupled from the caller's fd (the caller may close
    // it; the ring keeps the Spoor alive -- the I-30 submit-time-pin substrate).
    struct Spoor *spoors[LOOM_MAX_REG_HANDLES];
    rights_t      rights[LOOM_MAX_REG_HANDLES];
    u32 got = 0;
    for (u32 i = 0; i < n; i++) {
        struct Handle sh;
        if (handle_get(p, fds[i], &sh) != 0)   goto rollback;
        if (sh.kind != KOBJ_SPOOR)             { handle_put(&sh); goto rollback; }
        spoor_ref((struct Spoor *)sh.obj);
        spoors[got] = (struct Spoor *)sh.obj;
        rights[got] = sh.rights;
        got++;
        handle_put(&sh);
    }

    // loom_register_handles ADOPTS the `got` refs on success (it cannot fail
    // here: got <= n <= LOOM_MAX_REG_HANDLES).
    if (loom_register_handles(l, spoors, rights, got) != 0) goto rollback;
    handle_put(&lh);
    return 0;

rollback:
    for (u32 i = 0; i < got; i++) spoor_clunk(spoors[i]);
    handle_put(&lh);
    return -1;
}

int sys_loom_register_buffers_for_proc(struct Proc *p, hidx_t loom_fd,
                                       const struct loom_buf_reg *bufs, u32 n) {
    if (!p)                            return -1;
    if (n > LOOM_MAX_REG_BUFFERS)      return -1;
    if (n > 0 && !bufs)               return -1;

    struct Handle lh;
    if (handle_get(p, loom_fd, &lh) != 0)  return -1;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -1; }
    struct Loom *l = (struct Loom *)lh.obj;

    // handle_get holds a ref on the Loom across the call (the #844 by-value
    // snapshot with the obj ref held, table lock released), so loom_free cannot
    // run while loom_register_buffers walks p->vma_lock -- no lock nesting between
    // the handle-table lock and p->vma_lock.
    int rc = loom_register_buffers(l, p, bufs, n);
    handle_put(&lh);
    return rc;
}

static s64 sys_loom_setup_handler(u64 entries_raw, u64 params_va) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                              return -1;
    struct Proc *p = t->proc;
    if (!sys_validate_user_buf(params_va, sizeof(struct loom_params))) return -1;

    // Read the one IN field (params.flags, u32 at offset 0); the rest is OUT.
    u8 fb[4];
    for (int i = 0; i < 4; i++)
        if (uaccess_load_u8(params_va + (u64)i, &fb[i]) != 0) return -1;
    u32 flags = (u32)fb[0] | ((u32)fb[1] << 8) | ((u32)fb[2] << 16) | ((u32)fb[3] << 24);

    struct loom_params kp;
    hidx_t fd;
    if (sys_loom_setup_for_proc(p, (u32)entries_raw, flags, &kp, &fd) != 0) return -1;

    // Copy the geometry back. A fault mid-writeback fully rolls back the setup
    // (handle_close -> loom_unref drops the ring's handle_count; burrow_unmap
    // drops mapping_count to 0 -> the pages free) so a faulting caller never
    // leaks a ring it cannot see. ring_va / ring_size come from kp.
    const u8 *src = (const u8 *)&kp;
    for (u64 i = 0; i < sizeof(struct loom_params); i++) {
        if (uaccess_store_u8(params_va + i, src[i]) != 0) {
            (void)handle_close(p, fd);
            spin_lock(&p->vma_lock);
            (void)burrow_unmap(p, kp.ring_va, (size_t)kp.ring_size);
            spin_unlock(&p->vma_lock);
            return -1;
        }
    }
    return (s64)fd;
}

static s64 sys_loom_register_handler(u64 loom_fd_raw, u64 op_raw,
                                     u64 arg_va, u64 nargs_raw) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                              return -1;
    struct Proc *p = t->proc;
    u32 op = (u32)op_raw;
    u32 n  = (u32)nargs_raw;

    if (op == LOOM_REGISTER_HANDLES) {
        if (n > LOOM_MAX_REG_HANDLES)                return -1;
        hidx_t fds[LOOM_MAX_REG_HANDLES];
        if (n > 0) {
            if (!sys_validate_user_buf(arg_va, (u64)n * sizeof(u32))) return -1;
            for (u32 i = 0; i < n; i++) {
                u8 fb[4];
                for (int b = 0; b < 4; b++)
                    if (uaccess_load_u8(arg_va + (u64)i * 4u + (u64)b, &fb[b]) != 0)
                        return -1;
                u32 v = (u32)fb[0] | ((u32)fb[1] << 8) | ((u32)fb[2] << 16) | ((u32)fb[3] << 24);
                fds[i] = (hidx_t)v;
            }
        }
        return (s64)sys_loom_register_for_proc(p, (hidx_t)loom_fd_raw, op,
                                               n > 0 ? fds : NULL, n);
    }

    if (op == LOOM_REGISTER_BUFFERS) {
        if (n > LOOM_MAX_REG_BUFFERS)                return -1;
        struct loom_buf_reg bufs[LOOM_MAX_REG_BUFFERS];
        if (n > 0) {
            if (!sys_validate_user_buf(arg_va, (u64)n * sizeof(struct loom_buf_reg)))
                return -1;
            // Copy each {u64 va; u64 len} byte-by-byte (TOCTOU-safe; never re-read
            // after the kernel snapshot) and assemble little-endian.
            for (u32 i = 0; i < n; i++) {
                u64 base = arg_va + (u64)i * (u64)sizeof(struct loom_buf_reg);
                u8 raw[16];
                for (int b = 0; b < 16; b++)
                    if (uaccess_load_u8(base + (u64)b, &raw[b]) != 0) return -1;
                u64 va = 0, len = 0;
                for (int b = 0; b < 8; b++) {
                    va  |= (u64)raw[b]      << (8 * b);
                    len |= (u64)raw[8 + b]  << (8 * b);
                }
                bufs[i].va  = va;
                bufs[i].len = len;
            }
        }
        return (s64)sys_loom_register_buffers_for_proc(p, (hidx_t)loom_fd_raw,
                                                       n > 0 ? bufs : NULL, n);
    }

    return -1;   // unknown register op
}

int sys_loom_enter_for_proc(struct Proc *p, hidx_t loom_fd, u32 to_submit,
                            u32 min_complete, u32 flags) {
    if (!p) return -1;
    // handle_get holds a loom ref (loom_ref via handle_acquire_obj) for the whole
    // call, so loom_free cannot run concurrently with loom_enter -- the reap +
    // any submit run against a live ring; the abandon-before-free quiesce only
    // happens once this ref (and the table's) drop. handle_put drops it after.
    struct Handle lh;
    if (handle_get(p, loom_fd, &lh) != 0)  return -1;
    if (lh.kind != KOBJ_LOOM)              { handle_put(&lh); return -1; }
    int rc = loom_enter((struct Loom *)lh.obj, to_submit, min_complete, flags);
    handle_put(&lh);
    return rc;
}

static s64 sys_loom_enter_handler(u64 loom_fd_raw, u64 to_submit_raw,
                                  u64 min_complete_raw, u64 flags_raw) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                              return -1;
    return (s64)sys_loom_enter_for_proc(t->proc, (hidx_t)loom_fd_raw,
                                        (u32)to_submit_raw, (u32)min_complete_raw,
                                        (u32)flags_raw);
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

// Inner — testable kernel-internally with a Proc handle + a RESOLVED
// mount-point Spoor (stalk-2: the SVC wrapper stalk's the path; this inner
// does the source rights gate + flags check + the mount-table op). The mount
// table keys on the mount point's (dc, devno, qid.path) identity, extracted
// inside territory.c::mount -- the mountpoint Spoor is NOT retained.
int sys_mount_for_proc(struct Proc *p, hidx_t source_fd,
                       struct Spoor *mountpoint, u32 flags) {
    if (!p)                                          return -1;
    if (!p->territory)                               return -1;
    if (!mountpoint)                                 return -1;
    if (flags & ~SYS_MOUNT_VALID_FLAGS)               return -1;

    // RIGHT_READ on the source: a mount holder consumes the source's
    // tree (walks it, reads files through it). A handle without READ
    // is structurally useless as a mount source. RIGHT_TRANSFER is
    // separately required for cross-Proc transfer surfaces (Phase 5+),
    // not for the mount installation itself.
    struct Spoor *source = sys_lookup_spoor(p, source_fd, RIGHT_READ);
    if (!source)                                     return -1;

    // territory.c::mount handles: idempotency (no-op on duplicate), MREPL
    // (replace existing entry), full-table rejection, and the per-entry
    // spoor_ref. Returns 0 / -1 / -2 (table full) -> collapse to 0 / -1.
    // #844: source is REF-HELD (a borrow); mount() takes its own per-entry
    // ref, so release the borrow after.
    int rc = mount(p->territory, source, mountpoint, flags);
    spoor_clunk(source);
    return rc == 0 ? 0 : -1;
}

// Inner — testable kernel-internally. Returns 0 on success, -1 if no
// entry matches the mount point's identity.
int sys_unmount_for_proc(struct Proc *p, struct Spoor *mountpoint) {
    if (!p)                                          return -1;
    if (!p->territory)                               return -1;
    if (!mountpoint)                                 return -1;
    if (unmount(p->territory, mountpoint) != 0)       return -1;
    return 0;
}

// Resolve an absolute mount-point path from the caller's Territory root to its
// own Spoor identity (STALK_MOUNT: resolve, do NOT cross the final mount, do
// NOT open -- so a re-mount onto an already-mounted point keys on the SAME
// underlying identity and MREPL replaces it). Shared by SYS_MOUNT + UNMOUNT.
// Returns the owned mount-point Spoor (ref==1; the caller clunks it after the
// table op) or NULL on any failure. v1.0 resolves from root only (absolute
// paths); a relative-mount start_fd is a v1.x add.
static struct Spoor *sys_resolve_mountpoint(struct Proc *p,
                                            u64 path_va, u64 path_len_raw) {
    if (!p || !p->territory)                          return NULL;
    if (path_len_raw == 0)                            return NULL;
    if (path_len_raw > SYS_OPEN_PATH_MAX)             return NULL;
    if (!sys_validate_user_buf(path_va, path_len_raw)) return NULL;

    // RW-4 SA-F1: REF-HELD root (was a bare borrow -- a concurrent pivot_root
    // could free `start` while stalk walks it -> UAF). Clunked after stalk below
    // (and on every early-return path that follows).
    struct Spoor *start = territory_root_ref(p->territory);
    if (!start)                                       return NULL;

    // Copy + validate the path (reject embedded NUL -- truncation / wire-leak
    // vector). '/' is allowed (multi-component); stalk tokenizes it. One byte
    // over so the NUL terminator is always writable at the max length.
    char path_scratch[SYS_OPEN_PATH_MAX + 1];
    for (u64 i = 0; i < path_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(path_va + i, &b) != 0)    { spoor_clunk(start); return NULL; }
        if (b == '\0')                                { spoor_clunk(start); return NULL; }
        path_scratch[i] = (char)b;
    }
    path_scratch[path_len_raw] = '\0';

    struct Spoor *mp = stalk(p, start, path_scratch, path_len_raw, STALK_MOUNT, 0);
    spoor_clunk(start);   // SA-F1: release the root ref (stalk only borrowed it)
    return mp;
}

static s64 sys_mount_handler(u64 path_va, u64 path_len_raw,
                             u64 source_fd_raw, u64 flags_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (flags_raw > (u64)0xFFFFFFFFu)                 return -1;

    struct Spoor *mp = sys_resolve_mountpoint(p, path_va, path_len_raw);
    if (!mp)                                          return -1;

    s64 rc = (s64)sys_mount_for_proc(p, (hidx_t)source_fd_raw, mp,
                                     (u32)flags_raw);
    // territory.c::mount copied mp's identity, not mp itself -- release it.
    spoor_clunk(mp);
    return rc;
}

static s64 sys_unmount_handler(u64 path_va, u64 path_len_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    struct Spoor *mp = sys_resolve_mountpoint(p, path_va, path_len_raw);
    if (!mp)                                          return -1;

    s64 rc = (s64)sys_unmount_for_proc(p, mp);
    spoor_clunk(mp);
    return rc;
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
    // RW-3 R2-F1: reject len > SYS_RW_MAX -- do NOT silently cap. For a secret-
    // scrub primitive, capping + returning success would silently retain the
    // tail of the buffer; the libthyla-rs wrapper documents -1 on oversize, and
    // SYS_PUTS/SYS_READDIR reject oversize the same way.
    if (len > SYS_RW_MAX)                             return -1;
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
        // #844: snapshot + the obj ref. Take the child's long-lived spoor_ref
        // into bumped[], then handle_put releases the borrow. On get-fail
        // (hh zeroed -> no-op put) or kind-mismatch, put + unwind prior bumps.
        struct Handle hh;
        if (handle_get(p, (hidx_t)fds[i], &hh) < 0 || hh.kind != KOBJ_SPOOR) {
            handle_put(&hh);
            for (u32 j = 0; j < bumped_count; j++) spoor_unref(bumped[j]);
            return -1;
        }
        struct Spoor *s = (struct Spoor *)hh.obj;
        spoor_ref(s);
        bumped[bumped_count]        = s;
        bumped_rights[bumped_count] = hh.rights;
        bumped_count++;
        handle_put(&hh);
    }
    return 0;
}

// exec_load_from_namespace (#58) -- resolve the program `name` in the CALLER's
// namespace and slurp the ELF into a fresh 8-aligned kmalloc'd blob, replacing
// the flat boot-cpio `devramfs_lookup`. Realizes I-28 + I-1 for the exec path:
// a binary in a mounted FS (a container root, the disk-backed Stratum FS) is
// now executable, and a confined Proc can exec ONLY what its namespace names
// (the reverse-visibility leak closes -- a `stalk` miss is -1, never a flat
// fallback). Resolution mirrors SYS_OPEN exactly: an absolute path from the
// Territory `root_spoor`, a relative name via the LS-4 cwd-join; OEXEC gates a
// per-component X-search on every directory hop and PERM_R|PERM_X on the final
// file (RW-3 R3-F1), and the A-3 OEXEC->RIGHT_READ open yields a readable Spoor
// to load the bytes. The whole binary is read into kernel memory and handed
// UNCHANGED to exec_setup -- the same shape the cpio path used (it memcpy'd the
// whole blob), so the audited ELF loader / W^X reject / segment map are
// byte-identical. Runs in the parent's context (its Territory), like Unix exec.
// Returns the blob (caller kfree) + *size_out, or NULL on any failure.
// Exported for the kernel-internal #58 tests.
void *exec_load_from_namespace(struct Proc *p, const char *name,
                               size_t name_len, size_t *size_out) {
    if (!p || !name || !size_out)                  return NULL;
    *size_out = 0;
    if (name_len == 0 || name_len > SYS_OPEN_PATH_MAX) return NULL;

    // start = the Territory root (atomic ref under ns_lock, RW-4 SA-F1). An
    // absolute name resolves from it directly; a relative name is cwd-joined
    // (territory_resolve_cwd) into an absolute path first -- exactly SYS_OPEN.
    struct Spoor *start = territory_root_ref(p->territory);
    if (!start)                                    return NULL;

    char joined[SYS_OPEN_PATH_MAX + 1];
    const char *rpath = name;
    u64 rlen = (u64)name_len;
    if (name[0] != '/') {
        int jl = territory_resolve_cwd(p->territory, name, (u64)name_len,
                                       joined, sizeof(joined));
        if (jl < 0)                                { spoor_clunk(start); return NULL; }
        rpath = joined;
        rlen  = (u64)jl;
    }

    struct Spoor *quarry = stalk(p, start, rpath, rlen, STALK_OPEN, 3u /* OEXEC */);
    spoor_clunk(start);   // borrowed by stalk; release the ref we took
    if (!quarry)                                   return NULL;
    if (!quarry->dev || !quarry->dev->read)        { spoor_clunk(quarry); return NULL; }

    // Size from stat; bound by SYS_SPAWN_BLOB_MAX (the same cap the cpio path
    // enforced -- no regression; a > cap binary already failed before #58).
    struct t_stat st;
    if (spoor_stat_native(quarry, &st) != 0)       { spoor_clunk(quarry); return NULL; }
    if (st.size == 0 || st.size > SYS_SPAWN_BLOB_MAX)      { spoor_clunk(quarry); return NULL; }
    size_t sz = (size_t)st.size;

    void *blob = kmalloc(sz, 0);
    if (!blob)                                     { spoor_clunk(quarry); return NULL; }
    if (((uintptr_t)blob & 0x7) != 0)              { kfree(blob); spoor_clunk(quarry); return NULL; }

    // Read the whole file via dev->read (a short read is legal -- loop). For a
    // devramfs Spoor (the /bin bind) this is an in-memory copy; for dev9p it is
    // Tread round-trips (#811-death-interruptible if the parent dies). A read
    // that ends before st.size (truncated file) fails cleanly -> no partial map.
    u8 *dst = (u8 *)blob;
    s64 off = 0;
    size_t got = 0;
    while (got < sz) {
        long n = quarry->dev->read(quarry, dst + got, (long)(sz - got), off);
        if (n < 0)                                 { kfree(blob); spoor_clunk(quarry); return NULL; }
        if (n == 0)                                break;   // EOF before st.size
        got += (size_t)n;
        off += n;
    }
    spoor_clunk(quarry);
    if (got != sz)                                 { kfree(blob); return NULL; }

    *size_out = sz;
    return blob;
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

    // #58: resolve + slurp the ELF from the caller's namespace (was the flat
    // devramfs_lookup). The blob is 8-aligned, <= SYS_SPAWN_BLOB_MAX, kmalloc'd.
    size_t cpio_size = 0;
    void *blob_copy = exec_load_from_namespace(p, name, name_len, &cpio_size);
    if (!blob_copy)                                    return -1;

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

// Both spawn thunks call apply_spawn_perms (defined below, next to the grant
// gate); forward-declared here for the first caller.
void apply_spawn_perms(struct Proc *p, u32 perm_flags);

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
    // before exec_setup) rather than in the parent path so the child never
    // sees an un-stamped intermediate state — its very first user-mode
    // instruction observes the final state. The parent already gate-checked
    // every bit (sys_spawn_with_perms_for_proc); apply_spawn_perms maps each
    // surviving bit to its one-way kernel mark.
    apply_spawn_perms(p, perm_flags);

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

    // #58: resolve + slurp from the caller's namespace (was devramfs_lookup).
    size_t cpio_size = 0;
    void *blob_copy = exec_load_from_namespace(p, name, name_len, &cpio_size);
    if (!blob_copy) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

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

    // #58: resolve + slurp the ELF from the caller's namespace (was the flat
    // devramfs_lookup).
    size_t cpio_size = 0;
    void *blob_copy = exec_load_from_namespace(p, name, name_len, &cpio_size);
    if (!blob_copy)                                    return -1;

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

    // #58: resolve + slurp from the caller's namespace (was devramfs_lookup).
    size_t cpio_size = 0;
    void *blob_copy = exec_load_from_namespace(p, name, name_len, &cpio_size);
    if (!blob_copy) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

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

// spawn_perm_grant_check — the authoritative SPAWN_PERM_* grant gate, per-bit.
// SPAWN_PERM_CONSOLE_TRUSTED is the SAK trust anchor: console-attach-only, never
// delegable (so a service-poster can never confer the console-trust used for
// elevation -- I-27). SPAWN_PERM_MAY_POST_SERVICE (A-5b #827b) may be conferred by
// a console-attached granter OR by a Proc that ALREADY holds the bit -- the
// one-hop delegation (joey, console-attached, spawns /sbin/login WITH the bit;
// login confers it on a per-user --role client proxy that posts /srv/home-<user>
// in the session's private per-territory /srv). SPAWN_PERM_CONSOLE_OWNER (LS-5)
// is gated the SAME way as MAY_POST_SERVICE (console-attached OR a MAY_POST_SERVICE
// holder) so trusted /sbin/login confers console ownership on the session shell;
// it is strictly distinct from CONSOLE_TRUSTED (the owner bit never confers attach,
// so I-27 is untouched -- see the SPAWN_PERM_CONSOLE_OWNER comment in syscall.h).
// Every bit is rfork-non-propagating: a perm_flags spawn-time decision, not a
// cap_mask bit, so I-2 (the fork-grantable cap set only reduces) is unaffected.
// Returns 0 iff every requested bit may be granted by p; -1 otherwise. Both spawn
// entry points (SYS_SPAWN_WITH_PERMS and SYS_SPAWN_FULL_ARGV) route through here so
// the grant authority lives in exactly one place. Non-static so the kernel test
// suite can drive the per-bit decision directly on synthetic Procs.
int spawn_perm_grant_check(struct Proc *p, u32 perm_flags) {
    if (perm_flags & ~SPAWN_PERM_ALL)                              return -1;
    if ((perm_flags & SPAWN_PERM_CONSOLE_TRUSTED)
            && !proc_is_console_attached(p))                       return -1;
    if ((perm_flags & SPAWN_PERM_MAY_POST_SERVICE)
            && !proc_is_console_attached(p)
            && !proc_may_post_service(p))                          return -1;
    if ((perm_flags & SPAWN_PERM_CONSOLE_OWNER)
            && !proc_is_console_attached(p)
            && !proc_may_post_service(p))                          return -1;
    return 0;
}

// apply_spawn_perms — translate the parent-vetted SPAWN_PERM_* bits into their
// one-way kernel marks on the freshly-spawned child `p`. Called from both spawn
// thunks (sys_spawn_with_fds_thunk + sys_spawn_full_argv_thunk) in the CHILD
// thread's context, BEFORE exec_setup, so the child's first user-mode instruction
// observes the final state and never an un-stamped intermediate. The parent
// already gate-checked every bit (spawn_perm_grant_check); a bit outside
// SPAWN_PERM_ALL surviving to here is a kernel invariant violation. proc_set_*
// take g_proc_table_lock -- safe in the thunk (no lock held at this point).
// Non-static so the kernel test suite can drive the bit->action mapping directly
// (a real spawn races the child's exit clearing g_console_owner, so the owner-set
// wiring is unobservable through a full spawn).
void apply_spawn_perms(struct Proc *p, u32 perm_flags) {
    if (perm_flags & SPAWN_PERM_MAY_POST_SERVICE) {
        proc_mark_may_post_service(p);
    }
    if (perm_flags & SPAWN_PERM_CONSOLE_TRUSTED) {
        proc_set_console_trusted(p);   // A-4c-2: the SAK re-grant target
    }
    if (perm_flags & SPAWN_PERM_CONSOLE_OWNER) {
        proc_set_console_owner(p);     // LS-5: the session shell receives Ctrl-C
    }
    if (perm_flags & ~SPAWN_PERM_ALL) {
        extinction("apply_spawn_perms: unknown SPAWN_PERM_* bit");
    }
}

// SYS_SPAWN_WITH_PERMS — P5-corvus-srv-impl-b3a kernel body. The grant gate
// (spawn_perm_grant_check) is per-bit. Setting perm_flags=0 is identical to
// SYS_SPAWN_FULL — kept as a single entry point so callers that grant no
// permissions do not need a separate code path.
int sys_spawn_with_perms_for_proc(struct Proc *p,
                                  const char *name, size_t name_len,
                                  const u32 *fds, u32 fd_count,
                                  caps_t cap_mask, u32 perm_flags) {
    if (!p)                                             return -1;
    if (spawn_perm_grant_check(p, perm_flags) != 0)     return -1;
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

// Menagerie build-arc step 5: the parent-vetted hardware allowance carried
// from the SYS_SPAWN_FULL_ARGV handler into the child thunk (mirrors struct
// spawn_identity). When `set`, the thunk confers it via proc_confer_allowance
// before EL0; the arrays mirror struct Allowance's conferred set. The handler
// builds it (copy-in + count bound); the identity entry gates it as a narrowing
// vs the parent's allowance (allowance_confer_within_parent) before the body
// carries it here.
struct spawn_allowance {
    bool             set;
    struct hw_window mmio[ALLOWANCE_MMIO_MAX];
    u32              mmio_count;
    u32              irq[ALLOWANCE_IRQ_MAX];
    u32              irq_count;
    u64              dma_max;
    u32              pci[ALLOWANCE_PCI_MAX];
    u32              pci_count;
};

// The user-ABI struct t_allowance_desc uses fixed [8] arrays; pin them equal to
// the kernel allowance caps so the handler's copy-in cannot overflow the bundle
// (a future cap bump must bump the ABI struct + its asserts in lockstep).
_Static_assert(ALLOWANCE_MMIO_MAX == 8,
               "t_allowance_desc.mmio[8] mirrors ALLOWANCE_MMIO_MAX");
_Static_assert(ALLOWANCE_IRQ_MAX == 8,
               "t_allowance_desc.irq[8] mirrors ALLOWANCE_IRQ_MAX");
_Static_assert(ALLOWANCE_PCI_MAX == 8,
               "t_allowance_desc.pci[8] mirrors ALLOWANCE_PCI_MAX");

// Field-by-field copy of the allowance bundle. The kernel has no memcpy, so a
// whole-struct assignment would emit an undefined memcpy -- copy only the live
// entries [0..count) explicitly (each hw_window is a 16-byte copy clang
// inlines). Leaves dst's tail [count..MAX) untouched (never read).
static void spawn_allowance_copy(struct spawn_allowance *dst,
                                 const struct spawn_allowance *src) {
    dst->set        = src->set;
    dst->mmio_count = src->mmio_count;
    for (u32 i = 0; i < src->mmio_count && i < ALLOWANCE_MMIO_MAX; i++)
        dst->mmio[i] = src->mmio[i];
    dst->irq_count  = src->irq_count;
    for (u32 i = 0; i < src->irq_count && i < ALLOWANCE_IRQ_MAX; i++)
        dst->irq[i] = src->irq[i];
    dst->dma_max    = src->dma_max;
    dst->pci_count  = src->pci_count;
    for (u32 i = 0; i < src->pci_count && i < ALLOWANCE_PCI_MAX; i++)
        dst->pci[i] = src->pci[i];
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
    // Menagerie step 5: optional narrowed hardware allowance (conferred in the
    // thunk before exec when allowance.set; else the child keeps rfork's
    // inherited allowance -- NULL for a broad parent's child).
    struct spawn_allowance allowance;
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
    struct spawn_allowance allowance;                // step 5: copy before kfree
    spawn_allowance_copy(&allowance, &sa->allowance);
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

    // Apply parent-vetted SPAWN_PERM_* bits BEFORE anything user-observable;
    // the parent gate-checked them in sys_spawn_full_argv_for_proc. Same
    // one-way mapping as sys_spawn_with_fds_thunk (apply_spawn_perms).
    apply_spawn_perms(p, perm_flags);

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

    // Menagerie step 5: confer the parent-vetted narrowed hardware allowance
    // BEFORE any hw-handle create and before EL0 -- the proc_confer_allowance
    // set-once-before-EL0 contract holds (the child has no peer thread yet, so
    // no concurrent reader). The parent already gated it as a narrowing vs its
    // own allowance (allowance_confer_within_parent), so this only installs.
    // proc_confer_allowance frees any rfork-inherited clone. Fail-closed on OOM
    // (mirror the fd-install failure path: free the still-owned blob + argv).
    if (allowance.set) {
        if (proc_confer_allowance(p, allowance.mmio, allowance.mmio_count,
                                  allowance.irq, allowance.irq_count,
                                  allowance.dma_max,
                                  allowance.pci, allowance.pci_count) != 0) {
            // audit F2: no inherited fd is installed yet (the install loop is
            // below), so the bumped spoor refs in spoors_local[] would leak --
            // clunk all of them, mirroring the fail-fd-install arm's clunk of
            // its un-installed range.
            for (u32 j = 0; j < fd_count; j++) spoor_clunk(spoors_local[j]);
            kfree(blob);
            kfree(argv_data);
            exits("fail-allowance");
        }
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
        const struct spawn_identity *id,
        const struct spawn_allowance *want_allowance) {
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

    // #58: resolve + slurp from the caller's namespace (was devramfs_lookup).
    size_t cpio_size = 0;
    void *blob_copy = exec_load_from_namespace(p, name, name_len, &cpio_size);
    if (!blob_copy) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
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
    // Menagerie step 5: carry the parent-vetted allowance bundle (NULL ->
    // KP_ZERO left allowance.set false -> the child inherits via rfork).
    if (want_allowance) spawn_allowance_copy(&sa->allowance, want_allowance);
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

// A-1a: identity-aware entry — the real gate site. Does the per-bit SPAWN_PERM
// grant gate (spawn_perm_grant_check) AND the CAP_SET_IDENTITY gate (FAIL-CLOSED)
// + reserved-value reject, then delegates to the body. Exported (non-static) for
// kernel tests; the identity is passed as scalars (not the internal struct
// spawn_identity) so the test file needs no kernel-internal type. set_identity ==
// false (the back-compat path) means the child inherits the parent's identity.
int sys_spawn_full_argv_identity_for_proc(struct Proc *p,
        const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        bool set_identity, u32 principal_id, u32 primary_gid,
        const u32 *supp_gids, u32 supp_gid_count,
        const struct spawn_allowance *want_allowance) {
    if (!p)                                             return -1;
    if (spawn_perm_grant_check(p, perm_flags) != 0)     return -1;

    // Menagerie step 5: gate a conferred allowance as a NARROWING vs the
    // caller's OWN allowance (I-2's hardware-axis analog; allowance.tla). A
    // broad caller (the warden) may confer anything; a narrowed caller only a
    // subset of its own. No capability is needed to narrow. The thunk then
    // installs it (proc_confer_allowance) in the child before EL0. Runs in the
    // PARENT context, so `p` is the spawning Proc whose allowance bounds it.
    if (want_allowance && want_allowance->set) {
        if (!allowance_confer_within_parent(p, want_allowance->mmio,
                                            want_allowance->mmio_count,
                                            want_allowance->irq,
                                            want_allowance->irq_count,
                                            want_allowance->dma_max,
                                            want_allowance->pci,
                                            want_allowance->pci_count))
            return -1;
    }

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
                                                   cap_mask, perm_flags, eff_id,
                                                   want_allowance);
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
                                                 NULL, 0u,
                                                 /*want_allowance=*/NULL);
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
    // Menagerie step 5: allowance_flags must carry no unknown bits (forward-
    // compat, same rationale as _pad_envp); _pad_allow must be 0; a SET request
    // requires a non-NULL descriptor VA (the handler then copies + count-bounds
    // it). The mmio/irq count bounds + the narrowing gate are checked after the
    // copy-in / in the identity entry (so an over-count or a too-wide ask is a
    // clean -1, never a buffer overrun).
    if (req->allowance_flags & ~(u32)SPAWN_ALLOWANCE_FLAGS_ALL) return -1;
    if (req->_pad_allow != 0)                          return -1;
    if ((req->allowance_flags & SPAWN_ALLOWANCE_SET) &&
        req->allowance_va == 0)                        return -1;
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

    // Menagerie step 5: copy + count-bound the allowance descriptor when a SET
    // is requested (validate_req already checked allowance_va != 0). Bound the
    // counts BEFORE the copy loops so an over-count is a clean -1, never a
    // bundle overrun. The narrowing gate (vs the parent's allowance) runs in
    // the identity entry; here we only marshal the bytes.
    // Declared without a large {0} initializer (no memset in the kernel); the
    // SET path fills every field read downstream, the non-SET path passes NULL.
    struct spawn_allowance allow_kbuf;
    allow_kbuf.set = false;
    bool set_allowance = (req.allowance_flags & SPAWN_ALLOWANCE_SET) != 0;
    if (set_allowance) {
        if (!sys_validate_user_buf(req.allowance_va,
                                   sizeof(struct t_allowance_desc)))
            return -1;
        struct t_allowance_desc desc;
        u8 *ddst = (u8 *)&desc;
        for (u64 i = 0; i < sizeof(desc); i++) {
            u8 b = 0;
            if (uaccess_load_u8(req.allowance_va + i, &b) != 0) return -1;
            ddst[i] = b;
        }
        if (desc.mmio_count > ALLOWANCE_MMIO_MAX)      return -1;
        if (desc.irq_count > ALLOWANCE_IRQ_MAX)        return -1;
        if (desc.pci_count > ALLOWANCE_PCI_MAX)        return -1;
        allow_kbuf.set        = true;
        allow_kbuf.mmio_count = desc.mmio_count;
        for (u32 i = 0; i < desc.mmio_count; i++) {
            allow_kbuf.mmio[i].base = desc.mmio[i].base;
            allow_kbuf.mmio[i].size = desc.mmio[i].size;
        }
        allow_kbuf.irq_count = desc.irq_count;
        for (u32 i = 0; i < desc.irq_count; i++)
            allow_kbuf.irq[i] = desc.irq[i];
        allow_kbuf.dma_max = desc.dma_max;
        allow_kbuf.pci_count = desc.pci_count;
        for (u32 i = 0; i < desc.pci_count; i++)
            allow_kbuf.pci[i] = desc.pci[i];
    }

    return (s64)sys_spawn_full_argv_identity_for_proc(
        p, name, (size_t)req.name_len,
        req.argv_data_len > 0 ? argv_kbuf : NULL, req.argv_data_len, req.argc,
        fds_kbuf, req.fd_count,
        (caps_t)req.cap_mask, req.perm_flags,
        set_identity, req.principal_id, req.primary_gid,
        supp_kbuf, supp_count,
        set_allowance ? &allow_kbuf : NULL);
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

static s64 sys_wait_pid_handler(u64 want_pid_u, u64 flags_u, u64 status_out_va) {
    struct Thread *t = current_thread();
    if (!t)                                            return -1;
    struct Proc *p = t->proc;
    if (!p)                                            return -1;

    int want_pid = (int)(s64)want_pid_u;   // -1 (any) or a specific pid
    // Reject unknown/garbage flag bits (the full x1, before narrowing) so the
    // flag space stays clean for future additions and a fat-fingered flags
    // value fails loudly rather than silently behaving as blocking. Matches
    // the unknown-bit-reject discipline of the spawn / wstat surfaces.
    if (flags_u & ~(u64)WAIT_WNOHANG)                  return -1;
    int flags    = (int)flags_u;

    // Validate the destination buffer up-front (skipping if NULL) so a
    // bad user-VA doesn't cause a reap-then-fault hazard.
    if (status_out_va != 0) {
        if (!sys_validate_user_buf(status_out_va, sizeof(int))) return -1;
    }

    int status = 0;
    int reaped = wait_pid_for(want_pid, flags, &status);
    if (reaped < 0)                                    return -1;
    if (reaped == 0)                                   return 0;  // WAIT_WNOHANG: no zombie ready

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
    // #844: snapshot the slot (closes the torn-read TOCTOU vs a sibling close).
    // The KOBJ_SRV service obj is registry-owned (tombstoned at poster exit,
    // never freed by handle close -- handle_release_obj for it is a no-op), so
    // svc stays valid after handle_put; release the borrow immediately.
    struct Handle hh;
    if (handle_get(p, service_h, &hh) < 0)              return -1;
    if (hh.kind != KOBJ_SRV ||
        (hh.rights & RIGHT_READ) != RIGHT_READ ||
        !hh.obj ||
        *(const u64 *)hh.obj != SRV_SERVICE_MAGIC) {
        handle_put(&hh);
        return -1;
    }
    struct SrvService *svc = (struct SrvService *)hh.obj;
    handle_put(&hh);

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
    // #844: sp is REF-HELD (borrow); its last use is the CSRVCLIENT flag check
    // below, after which cn (kernel-owned, value-captured identity) carries the
    // rest. spoor_clunk on every exit through there, then once after.
    struct Spoor *sp = sys_lookup_spoor(p, conn_h, RIGHT_READ);
    if (!sp) return -1;

    // The Spoor must be a devsrv connection Spoor; devsrv_conn_of returns
    // NULL for a pipe / dev9p / devsrv-root / devsrv-service Spoor.
    struct SrvConn *cn = devsrv_conn_of(sp);
    if (!cn) { spoor_clunk(sp); return -1; }

    // SO_PEERCRED is an ACCEPT-side (server) query -- "who connected to me?".
    // stalk-3c open=connect made the CLIENT endpoint a devsrv conn Spoor too
    // (CSRVCLIENT), so devsrv_conn_of now resolves it; but the SrvConn stamps
    // the CONNECTOR as the peer, so a client-side query would mis-report the
    // caller's OWN identity (and in a same-Proc client+server the poster gate
    // below cannot tell them apart). Reject the client endpoint: SYS_SRV_PEER
    // is server-side only at v1.0 (pouch getsockopt(SO_PEERCRED) -> ENOTSOCK).
    if (sp->flag & CSRVCLIENT) { spoor_clunk(sp); return -1; }

    // #844 audit F2: capture EVERY cn-derived value while the borrow (sp) still
    // pins cn (cn = sp->aux is raw, NOT independently refcounted), THEN drop the
    // borrow. Reading cn after spoor_clunk(sp) would be a UAF if a sibling
    // closed the server fd in between (sp's table ref -> devsrv_close ->
    // srvconn_unref -> cn freed). All three are value-captured-at-mint fields,
    // so this is a pure hoist -- no semantic change.
    u64  server_stripes = srvconn_server_stripes(cn);
    u64  peer_stripes   = srvconn_peer_stripes(cn);
    bool peer_console   = srvconn_peer_console(cn);
    spoor_clunk(sp);

    // Poster gate (CORVUS-DESIGN §6.3): only the service's poster may query a
    // connection's peer. The SrvConn captured the poster's stripes by value at
    // mint; the caller's stripes must match.
    u64 caller = proc_stripes(p);
    if (caller == 0)                            return -1;
    if (server_stripes != caller)               return -1;

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
// A-5a: boot -> session transition syscalls (login + session lifecycle).
// IDENTITY-DESIGN.md section 9.9 + the ARCH section 25.4 "A-5" audit-trigger row.
// =============================================================================

// SYS_BOOT_COMPLETE -- init (joey) signals its boot-test asserts passed. Prints
// the "Thylacine boot OK" banner exactly once (boot_mark_complete is one-shot).
// GATE: the caller must be console-attached -- only the boot console-trust
// anchor (joey, pre-relinquish) can emit the banner, so a spawned child cannot
// fake a premature banner (-> a false test PASS). joey persists after this
// (getty-loops login), so the banner can no longer ride joey's reap.
static s64 sys_boot_complete_handler(void) {
    struct Thread *t = current_thread();
    if (!t)                            return -1;
    struct Proc *p = t->proc;
    if (!p)                            return -1;
    if (!proc_is_console_attached(p))  return -1;
    (void)boot_mark_complete();
    return 0;
}

// SYS_CONSOLE_RELINQUISH -- the caller drops its OWN console-attach (I-27). joey
// calls this at the bringup->session boundary so corvus becomes the SOLE
// console-attached Proc during a user session. Self-only (passes the caller's
// Proc -- it cannot revoke another Proc); gated on the caller currently being
// console-attached (can only relinquish what you hold).
static s64 sys_console_relinquish_handler(void) {
    struct Thread *t = current_thread();
    if (!t)                            return -1;
    struct Proc *p = t->proc;
    if (!p)                            return -1;
    if (!proc_is_console_attached(p))  return -1;
    proc_console_relinquish(p);
    return 0;
}

// SYS_CONSOLE_OPEN core -- attach /dev/cons + install a KOBJ_SPOOR R|W handle.
// The getty (joey) hands this to /sbin/login as its tty (fd 0/1/2; the Unix
// login-reads-the-tty model). devcons_read ignores the Spoor and drains the
// global RX ring, so any opened handle is a valid console reader. Exposed
// (non-static) for kernel-internal tests (test_cons.c). Returns the fd or -1;
// on handle-table failure the Spoor ref taken by attach is released.
hidx_t sys_console_open_for_proc(struct Proc *p) {
    if (!p)                            return -1;
    struct Spoor *cs = devcons.attach(NULL);   // dev_simple_attach -> ref=1
    if (!cs)                           return -1;
    hidx_t fd = handle_alloc(p, KOBJ_SPOOR, RIGHT_READ | RIGHT_WRITE, cs);
    if (fd < 0) {
        spoor_clunk(cs);
        return -1;
    }
    return fd;
}

static s64 sys_console_open_handler(void) {
    struct Thread *t = current_thread();
    if (!t)                            return -1;
    struct Proc *p = t->proc;
    if (!p)                            return -1;
    // A-5a audit F2: gate on console-attach. /dev/cons is a single-reader global
    // (devcons_read drains one ring; first reader wins), so an UNGATED open lets
    // any EL0 Proc (a user shell's child) steal the getty's console input or deny
    // it a reader slot. Only the console-trust anchor may open it: joey opens it
    // while still attached (BEFORE SYS_CONSOLE_RELINQUISH) and hands it to login;
    // post-SAK corvus is the attached Proc. login + the user shell are never
    // console-attached, so they cannot open /dev/cons.
    if (!proc_is_console_attached(p))  return -1;
    return (s64)sys_console_open_for_proc(p);
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

    case SYS_PCI_CLAIM:
        ctx->regs[0] = (u64)sys_pci_claim_handler(ctx->regs[0],
                                                  ctx->regs[1]);
        return;

    case SYS_PCI_MAP_BAR:
        ctx->regs[0] = (u64)sys_pci_map_bar_handler(ctx->regs[0],
                                                    ctx->regs[1],
                                                    ctx->regs[2],
                                                    ctx->regs[3]);
        return;

    case SYS_PCI_INFO:
        ctx->regs[0] = (u64)sys_pci_info_handler(ctx->regs[0],
                                                 ctx->regs[1]);
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
        ctx->regs[0] = (u64)sys_mount_handler(ctx->regs[0],   // path_va
                                              ctx->regs[1],   // path_len
                                              ctx->regs[2],   // source_fd
                                              ctx->regs[3]);  // flags
        return;

    case SYS_UNMOUNT:
        ctx->regs[0] = (u64)sys_unmount_handler(ctx->regs[0],   // path_va
                                                ctx->regs[1]);  // path_len
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
        ctx->regs[0] = (u64)sys_wait_pid_handler(ctx->regs[0], ctx->regs[1], ctx->regs[2]);
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

    case SYS_BOOT_COMPLETE:
        ctx->regs[0] = (u64)sys_boot_complete_handler();
        return;

    case SYS_CONSOLE_RELINQUISH:
        ctx->regs[0] = (u64)sys_console_relinquish_handler();
        return;

    case SYS_CONSOLE_OPEN:
        ctx->regs[0] = (u64)sys_console_open_handler();
        return;

    case SYS_WALK_OPEN:
        ctx->regs[0] = (u64)sys_walk_open_handler(ctx->regs[0],
                                                  ctx->regs[1],
                                                  ctx->regs[2],
                                                  ctx->regs[3]);
        return;

    case SYS_OPEN:
        ctx->regs[0] = (u64)sys_open_handler(ctx->regs[0],
                                             ctx->regs[1],
                                             ctx->regs[2],
                                             ctx->regs[3]);
        return;

    case SYS_CHDIR:
        ctx->regs[0] = (u64)sys_chdir_handler(ctx->regs[0],
                                              ctx->regs[1],
                                              ctx->regs[2],
                                              ctx->regs[3]);
        return;

    case SYS_GETCWD:
        ctx->regs[0] = (u64)sys_getcwd_handler(ctx->regs[0],
                                               ctx->regs[1],
                                               ctx->regs[2],
                                               ctx->regs[3]);
        return;

    case SYS_FD2PATH:
        ctx->regs[0] = (u64)sys_fd2path_handler(ctx->regs[0],
                                                ctx->regs[1],
                                                ctx->regs[2],
                                                ctx->regs[3]);
        return;

    case SYS_GETPID:
        ctx->regs[0] = (u64)sys_getpid_handler(ctx->regs[0], ctx->regs[1],
                                               ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_GETUID:
        ctx->regs[0] = (u64)sys_getuid_handler(ctx->regs[0], ctx->regs[1],
                                               ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_GETGID:
        ctx->regs[0] = (u64)sys_getgid_handler(ctx->regs[0], ctx->regs[1],
                                               ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_CLOCK_GETTIME:
        ctx->regs[0] = (u64)sys_clock_gettime_handler(ctx->regs[0], ctx->regs[1],
                                                      ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_CLOCK_SETTIME:
        ctx->regs[0] = (u64)sys_clock_settime_handler(ctx->regs[0], ctx->regs[1],
                                                      ctx->regs[2], ctx->regs[3]);
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

    case SYS_LOOM_SETUP:
        ctx->regs[0] = (u64)sys_loom_setup_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_LOOM_REGISTER:
        ctx->regs[0] = (u64)sys_loom_register_handler(ctx->regs[0], ctx->regs[1],
                                                      ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_LOOM_ENTER:
        ctx->regs[0] = (u64)sys_loom_enter_handler(ctx->regs[0], ctx->regs[1],
                                                   ctx->regs[2], ctx->regs[3]);
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
                                                     ctx->regs[3],
                                                     ctx->regs[4]);
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
