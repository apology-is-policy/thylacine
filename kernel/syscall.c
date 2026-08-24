// Userspace syscall dispatch implementation (P3-Ec).
//
// At v1.0 P3-Ec the syscall surface is intentionally tiny — exits +
// puts. Just enough for an EL0 thread to signal "I ran, and here's the
// result" to the kernel test harness. Phase 5+ adds the full syscall
// surface; each syscall lands in its own TU.

#include <thylacine/syscall.h>
#include <thylacine/9p_attach.h>
#include <thylacine/9p_client.h>          // p9_client_weft (SYS_WEFT_MAP; Weft-6a-2)
#include <thylacine/9p_spoor_transport.h>
#include <thylacine/9p_srvconn_transport.h>
#include <thylacine/9p_wire.h>            // struct p9_weft_geom (Weft-6a-2)
#include <thylacine/burrow.h>
#include <thylacine/caps.h>
#include <thylacine/cons.h>               // cons_output_write (SYS_PUTS; #76)
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
#include <thylacine/image.h>           // D-3: the FILE mmap arm shares exec's Image cache
#include <thylacine/irqfwd.h>
#include <thylacine/loom.h>
#include <thylacine/mmio_handle.h>
#include <thylacine/notes.h>
#include <thylacine/page.h>
#include <thylacine/path.h>    // struct Path (SYS_FD2PATH reads ->s/->len; #66)
#include <thylacine/pts.h>     // the pts registry (SYS_PTY_REGISTER; PTY-1c)
#include <thylacine/pci_handle.h>   // KObj_PCI (SYS_PCI_CLAIM/MAP_BAR/INFO; pci-1c)
#include "../arch/arm64/mmu.h"      // V-2: MAIR_IDX_* for SYS_BURROW_FROM_HOSTMEM
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
#include <thylacine/vivarium.h>         // the Linux translation table (V-1b branch)
#include <thylacine/vma.h>
#include <thylacine/weft.h>             // share_id registry + binding (SYS_WEFT_*; Weft-6a-2)

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

// #76: SYS_PUTS routes through cons_output_write -- the ONE console-output
// implementation (#57b) -- instead of its own byte-by-byte uart_putc loop.
//
// The loop was the pre-P1-F shape #75 exists to eliminate, left behind in this
// caller when P1-F converted cons_output_write. SYS_PUTS is the native
// diagnostic channel (83 binaries reach it via libthyla_rs::t_putstr /
// libt::t_puts), so the defect was not niche -- it carried TWO:
//
//   1. NO WRITER ROLE. uart_putc is lock-free, so a SYS_PUTS interleaved at
//      BYTE granularity with a concurrent /dev/cons writer -- which does hold
//      the role -- and with peer SYS_PUTS callers. Observed live: an LS-CI
//      login prompt came out as `patapestrssyd: mworodd:e`, i.e. "password: "
//      (login, via fd 1 -> cons_output_write) shredded byte-for-byte against
//      "tapestryd: mode " (tapestryd, via t_putstr). A role that only some
//      writers take excludes nobody.
//
//   2. NO DRAIN TAP. cons_drain_tap fires from cons_emit / cons_emit_wait
//      only, so nothing written via SYS_PUTS ever reached the G-4 renderer:
//      the whole native diagnostic stream was INVISIBLE on the graphical
//      console while appearing normally on serial.
//
// Routing here fixes both at once, and the ONLCR that comes with it is
// required rather than incidental: (2) newly exposes this output to aurora,
// whose VT does not synthesize CR on LF (#36), so bare-LF writes would
// staircase the moment they became visible.
//
// The copy-in happens BEFORE the role is claimed, deliberately. Faulting a
// user page can sleep, and holding the console role across an unbounded
// page-in would stall every other console writer behind it; staging first
// bounds the role to the emit. This is the CF-3 A byte-I/O staging shape
// (`u8 scratch[SYS_RW_STACK]` + one bulk uaccess), and SYS_PUTS's existing
// 4096 cap already equals SYS_RW_STACK -- naming the constant makes the
// buffer and the cap the same fact instead of two that agree today.
static s64 sys_puts_handler(u64 buf_va, u64 len) {
    if (len == 0)            return 0;
    if (len > SYS_RW_STACK)  return -1;
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

    // Stage the whole buffer into kernel memory first. uaccess_copy_in
    // returns 0, or -1 if the fault dispatcher couldn't demand-page the
    // user VA (no VMA / perm denied / OOM during sub-table alloc); on -1
    // the buffer's tail is unspecified, so -1 is a whole-op EFAULT here
    // and nothing is emitted. Emitting nothing on a partial fault is a
    // deliberate improvement on the old loop, which pushed the readable
    // prefix to the console before failing.
    u8 scratch[SYS_RW_STACK];
    if (uaccess_copy_in(scratch, buf_va, len) != 0) return -1;

    // Role-held, ring-buffered, drain-tapped, ONLCR-cooked -- identical
    // treatment to a /dev/cons write, which is the point: one console,
    // one writer discipline.
    //
    // The return may now be SHORT (< len) where it was previously len-or-
    // -1: cons_output_write cuts a write off on the #67 stalled-consumer
    // deadline or a #811 death, and reporting that honestly is strictly
    // better than claiming bytes we dropped. No caller inspects the count
    // (every t_puts/t_putstr use in usr/ is for side effect), so this
    // widens the contract without breaking a consumer. A negative return
    // stays -1 so the "any negative is failure" reading is unchanged.
    long w = cons_output_write(scratch, (long)len);
    return (w < 0) ? -1 : (s64)w;
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
    // P6 #713 vma_lock audit F1: burrow_map walks + splices p->as->vmas
    // (vma_insert), so it MUST hold p->vma_lock -- same discipline as
    // SYS_BURROW_ATTACH. Without it a sibling thread's fault-path
    // vma_lookup (or another mapper) races this vma_insert. Lock order
    // vma_lock -> buddy zone->lock holds (burrow_unref-on-failure ->
    // free_pages). burrow_create_mmio stays outside (no VMA touch).
    spin_lock(&p->as->lock);
    int rc = burrow_map(p, b, vaddr, km->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        spin_unlock(&p->as->lock);
        handle_put(&hh);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->as->lock);
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
// SYS_DMA_CREATE_WEAVE — mint a share-admissible device-passive DMA weave (G-2).
// =============================================================================
//
// AArch64 ABI: x0 = size, x1 = rights. The TAPESTRY.md §18.1 weave-backing
// mint (ABI user-signed-off 2026-07-19): byte-for-byte the SYS_DMA_CREATE
// contract — the SAME CAP_HW_CREATE gate, the SAME I-34 allowance
// CreateBegin/CreateCommit pair (a narrowed driver's dma_max bounds weave
// creates identically) — differing only in the size envelope
// (KOBJ_DMA_WEAVE_MAX_SIZE) and the kernel-minted `weave` subtype bit on the
// returned KObj_DMA. See the syscall.h doc block for why this is a separate
// number rather than a flags widening of SYS_DMA_CREATE.
static s64 sys_dma_create_weave_handler(u64 size, u64 rights) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;
    if (rights == 0 || (rights & ~(u64)RIGHT_ALL))   return -1;
    if (size == 0)                                   return -1;

    // I-34 CreateBegin (the sys_dma_create_handler discipline): the allowance
    // dma_max axis sees the FULL weave size — a narrowed driver cannot mint a
    // weave larger than its conferred per-buffer bound.
    if (!allowance_permits(p, HW_RES_DMA, size, 0))  return -1;

    struct KObj_DMA *k = kobj_dma_create_weave((size_t)size);
    if (!k)                                          return -1;

    // I-34 CreateCommit: install under the allowance re-check (revoke race).
    hidx_t h = allowance_handle_alloc(p, KOBJ_DMA, (rights_t)rights, k);
    if (h < 0) {
        kobj_dma_unref(k);
        return -1;
    }
    return (s64)h;
}

// =============================================================================
// SYS_DMA_CREATE_GPU_BO — mint a share-admissible GPU buffer (Warp-2, §6.1).
// =============================================================================
//
// AArch64 ABI: x0 = size, x1 = rights. Byte-for-byte the weave handler above
// -- the SAME CAP_HW_CREATE gate + I-34 CreateBegin/CreateCommit pair --
// differing only in the envelope and the minted subtype bit. See the
// syscall.h doc block for the separate-number rationale and the GPU BO's
// distinct device-WRITTEN safety argument.
static s64 sys_dma_create_gpu_bo_handler(u64 size, u64 rights) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;
    if (rights == 0 || (rights & ~(u64)RIGHT_ALL))   return -1;
    if (size == 0)                                   return -1;

    // I-34 CreateBegin: the allowance dma_max axis sees the FULL BO size -- a
    // narrowed driver cannot mint a BO larger than its conferred bound.
    if (!allowance_permits(p, HW_RES_DMA, size, 0))  return -1;

    struct KObj_DMA *k = kobj_dma_create_gpu_bo((size_t)size);
    if (!k)                                          return -1;

    // I-34 CreateCommit: install under the allowance re-check (revoke race).
    hidx_t h = allowance_handle_alloc(p, KOBJ_DMA, (rights_t)rights, k);
    if (h < 0) {
        kobj_dma_unref(k);
        return -1;
    }
    return (s64)h;
}

// V-2 (audit F2): resolve + bounds-check a hostmem BAR subrange. Pure (no Proc /
// handle state) so the security-load-bearing shm-scan + OOB rejects + base_pa
// arithmetic are unit-testable directly (test_weft_hostmem_resolve). Returns 0 +
// *base_pa_out on success; -1 on a bad shmid, a zero/unaligned length, a miss, or
// an OOB subrange. Non-wrapping bounds: offset <= shm.length and length <=
// shm.length - offset, and discovery pins shm.offset + shm.length <= bar.size,
// so base_pa + length never escapes the BAR (I-45).
int hostmem_resolve_subrange(const struct KObj_PCI *k, u64 shmid, u64 offset,
                             u64 length, u64 *base_pa_out);
int hostmem_resolve_subrange(const struct KObj_PCI *k, u64 shmid, u64 offset,
                             u64 length, u64 *base_pa_out) {
    if (!k || !base_pa_out)                          return -1;
    if (length == 0)                                 return -1;
    if (shmid > 0xffu)                               return -1;   // shmid is a u8
    if ((offset & (PAGE_SIZE - 1)) || (length & (PAGE_SIZE - 1))) return -1;
    for (u32 s = 0; s < PCI_SHM_COUNT; s++) {
        if (!k->shm[s].present || k->shm[s].shmid != (u8)shmid) continue;
        u8 bar = k->shm[s].bar;
        if (bar >= PCI_BAR_COUNT || !k->bars[bar].present)      return -1;  // malformed
        if (offset > k->shm[s].length)                          return -1;  // OOB start
        if (length > k->shm[s].length - offset)                 return -1;  // OOB extent
        *base_pa_out = k->bars[bar].pa + k->shm[s].offset + offset;
        return 0;
    }
    return -1;   // no window with this shmid
}

// =============================================================================
// SYS_BURROW_FROM_HOSTMEM — mint a Burrow over a PCI hostmem BAR subrange and
// map it into the caller (Warp-6 V-2; GPU-DESIGN §6.2.1).
// =============================================================================
//
// Authority is owning the pci_handle claim: KOBJ_PCI is in KOBJ_KIND_HW_MASK
// (I-5 non-transferable), so a KOBJ_PCI handle in p's table IS proof p owns the
// claim -- no CAP_HW_CREATE (unlike the *_CREATE mints, which forge a NEW hw
// object; this only re-exposes memory the claim already owns). I-45: the map
// reaches only the named BAR subrange of the caller's own claim, at a
// cacheable/NC attribute conveying zero hardware authority. I-32: the caller's
// page-budget is NOT charged (BAR pages, not RAM); a CLIENT that later maps this
// via burrow_share_into is charged its own shared_map_pages.
static s64 sys_burrow_from_hostmem_handler(u64 pci_hraw, u64 shmid, u64 offset,
                                           u64 length, u64 cache_policy) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // ABI cache policy -> kernel MAIR index (the boundary translation).
    u8 mair_idx;
    switch (cache_policy) {
    case T_CACHE_CACHED:                mair_idx = MAIR_IDX_NORMAL_WB; break;
    case T_CACHE_WC:
    case T_CACHE_UNCACHED:              mair_idx = MAIR_IDX_NORMAL_NC; break;
    default:                            return -1;
    }

    // Resolve + validate ownership of the PCI claim (the sys_pci_map_bar_handler
    // block). The held handle keeps k alive across burrow_create_hostmem (which
    // takes its OWN kobj_pci ref); handle_put balances every exit.
    struct Handle hh;
    if (handle_get(p, (hidx_t)pci_hraw, &hh) < 0)    return -1;
    if (hh.kind != KOBJ_PCI)            { handle_put(&hh); return -1; }
    if ((hh.rights & RIGHT_MAP) == 0)   { handle_put(&hh); return -1; }
    struct KObj_PCI *k = (struct KObj_PCI *)hh.obj;
    if (!k)                             { handle_put(&hh); return -1; }
    if (k->magic != KOBJ_PCI_MAGIC)     { handle_put(&hh); return -1; }

    // Resolve + bounds-check the subrange (F2: the pure, unit-tested core --
    // shmid select, page-align + OOB rejects, base_pa = bar.pa + window offset +
    // caller offset, bounded within the BAR).
    u64 base_pa;
    if (hostmem_resolve_subrange(k, shmid, offset, length, &base_pa) != 0) {
        handle_put(&hh);
        return -1;
    }

    // Mint the hostmem Burrow (handle_count=1 construction ref). base_pa must be
    // page-aligned (bars[].pa is; offset is; a non-page-aligned window offset is
    // rejected inside create).
    struct Burrow *b = burrow_create_hostmem(k, base_pa, (size_t)length, mair_idx);
    if (!b)                             { handle_put(&hh); return -1; }

    // Place + map into the caller's burrow-attach window (auto VA, the weft-map
    // shape). burrow_map takes the mapping ref; drop the construction ref so the
    // VMA owns the Burrow (mapping_count=1 keeps it alive). The whole
    // find-gap -> map runs under p->as->lock (the #713 vmas-mutator rule).
    spin_lock(&p->as->lock);
    u64 va;
    if (vma_find_gap(p, (size_t)length, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &va) != 0) {
        spin_unlock(&p->as->lock);
        burrow_unref(b);
        handle_put(&hh);
        return -1;
    }
    if (burrow_map(p, b, va, (size_t)length, VMA_PROT_RW) != 0) {
        spin_unlock(&p->as->lock);
        burrow_unref(b);
        handle_put(&hh);
        return -1;
    }
    burrow_unref(b);                    // drop construction ref; VMA owns it
    spin_unlock(&p->as->lock);
    handle_put(&hh);
    return (s64)va;
}

// =============================================================================
// SYS_HOSTMEM_REFCOUNT — read a hostmem Burrow's TOTAL #847 reference count
// (handle_count + mapping_count) (Warp-6 V-3b-1c-2b F2; WARP-V3-DESIGN.md 0.11.3).
// =============================================================================
//
// Read-only. Resolves [va, va+len) to a SINGLE BURROW_TYPE_HOSTMEM VMA the
// CALLER owns (vma_lookup searches only p's own AddrSpace, so ownership is
// structural -- a caller can only count a hostmem burrow it maps) and returns
// handle_count + mapping_count: EVERY live reference on the host-visible ring
// backing -- the caller's own mapping PLUS any weft-shared client's mapping AND
// its transferred registration pin (a handle_count ref). Because the caller maps
// the burrow, the sum is always >= 1, and == 1 iff the ONLY reference is the
// caller's single mapping. This is image.c's kernel-side eviction predicate
// (handle_count==1 && mapping_count==0) folded to one value for the tapestryd
// side, where the caller holds the mapping instead of the handle.
//
// Why the SUM, not mapping_count alone (the audit F1 correction): a client's map
// is COMMITTED at weft_share_claim (weft.c), which consumes the share and
// TRANSFERS the registration pin to the client -- a handle_count ref -- and
// returns BEFORE burrow_share_into bumps mapping_count later in the same
// SYS_WEFT_MAP. In that window a client is irrevocably going to map GPA(off) yet
// mapping_count still reads 1. A reclaim keyed on mapping_count==1 would free the
// offset under that pending map (a cross-client alias). The transferred pin makes
// the sum >= 2, so the SUM excludes the in-flight claimant exactly as image.c's
// handle_count half does.
//
// It leaks nothing but the number: no kernel address, no other Proc, no per-kind
// breakdown. Refuses (-T_E_INVAL) a va that does not resolve, a VMA not fully
// covering [va, va+len), or a non-HOSTMEM burrow.
//
// Still a racy ACQUIRE snapshot (burrow.c "Diagnostics"); it is a SAFE reclaim
// basis ONLY under tapestryd's ref-discipline: disarm the weft share FIRST (so no
// NEW claim can consume it), then at sum==1 the only reference is the caller's
// map, which cannot grow -- no share to claim, no existing client ref (map OR
// pin) to fork. Read under p->as->lock so a concurrent same-Proc detach cannot
// splice the VMA->Burrow link mid-read; both counts snapshot under it.
//
// The core is separated from the current_thread() wrapper so the resolve +
// type-gate + range logic is unit-testable with a synthetic Proc (the
// hostmem_resolve_subrange precedent). NOT static: the test declares it.
s64 hostmem_refcount_query(struct Proc *p, u64 va, u64 len) {
    if (!p || !p->as)       return -T_E_INVAL;
    if (len == 0)           return -T_E_INVAL;
    if (va + len < va)      return -T_E_INVAL;   // range wrap

    spin_lock(&p->as->lock);
    struct Vma *vma = vma_lookup(p, va);
    if (!vma || va < vma->vaddr_start || va + len > vma->vaddr_end) {
        spin_unlock(&p->as->lock);
        return -T_E_INVAL;
    }
    struct Burrow *b = vma->burrow;
    if (!b || b->type != BURROW_TYPE_HOSTMEM) {
        spin_unlock(&p->as->lock);
        return -T_E_INVAL;
    }
    // Both counts are ACQUIRE + magic-checked; summed under as->lock so a
    // same-Proc detach cannot drop the mapping between the two reads.
    s64 refs = (s64)burrow_handle_count(b) + (s64)burrow_mapping_count(b);
    spin_unlock(&p->as->lock);
    return refs;
}

static s64 sys_hostmem_refcount_handler(u64 va, u64 len) {
    struct Thread *t = current_thread();
    if (!t) return -T_E_INVAL;
    return hostmem_refcount_query(t->proc, va, len);
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
    // P6 #713 vma_lock audit F1: burrow_map mutates p->as->vmas (vma_insert),
    // so it MUST hold p->vma_lock -- same discipline as SYS_BURROW_ATTACH /
    // SYS_MMIO_MAP. stratumd (multi-thread, CAP_HW_CREATE) maps its
    // virtio-blk DMA buffer here concurrently with sibling-thread faults.
    spin_lock(&p->as->lock);
    int rc = burrow_map(p, b, vaddr, kd->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        spin_unlock(&p->as->lock);
        handle_put(&hh);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->as->lock);

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

    // The arg packs two fields (G-7c): low 32 = the virtio device id (a u16
    // on the wire; the id half must fit), high 32 = the 0-based INSTANCE
    // ordinal selecting the nth same-id function in enumeration order.
    // Every pre-G-7c caller passes a bare id (typed u64 wrappers zero the
    // high word by construction) -> nth 0 -> the historical first-match
    // behavior, byte-identical. An over-large nth resolves no device (-1).
    u32 id  = (u32)(virtio_device_id & 0xFFFFFFFFu);
    u32 nth = (u32)(virtio_device_id >> 32);
    if (id > 0xFFFFu)                                return -1;

    // I-34 CreateBegin (specs/allowance.tla; build-arc step 6): SYS_PCI_CLAIM is
    // the fourth hw-handle-minting path. Resolve (id, nth) -> (bus,dev,fn)
    // read-only (the SAME nth match kobj_pci_claim will pick -- the device
    // table is boot-built + immutable, so the pair is stable across the
    // resolve->claim window), then gate it against the calling Proc's
    // per-(bus,dev,fn) PCI allowance axis. A broad Proc (the warden + the
    // trusted servers, allowance == NULL) passes; a NARROWED driver may claim
    // only a function the warden conferred -- closing the bypass where a
    // driver narrowed to one device could claim another's PCI function ("a
    // PCI device's allowance IS its claimed BARs", MENAGERIE.md §4). Gating
    // on the resolved bdf BEFORE the claim means a not-permitted device is
    // never enabled (MEM-decode + bus-master) only to be rolled back.
    u8 bus, dev, fn;
    if (kobj_pci_resolve_bdf(id, nth, &bus, &dev, &fn) != 0)
        return -1;
    if (!allowance_permits(p, HW_RES_PCI, PCI_BDF_PACK(bus, dev, fn), 0))
        return -1;

    struct KObj_PCI *k = kobj_pci_claim(id, nth);
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

    // burrow_map walks + splices p->as->vmas, so it holds p->vma_lock (the #713
    // discipline). Lock order vma_lock -> buddy zone->lock holds. km->size is the
    // full decoded BAR size; the user maps the whole BAR and indexes the
    // VIRTIO_PCI_CAP regions within it.
    spin_lock(&p->as->lock);
    int rc = burrow_map(p, b, vaddr, km->size, prot);
    if (rc < 0) {
        burrow_unref(b);
        spin_unlock(&p->as->lock);
        handle_put(&hh);
        return -1;
    }
    burrow_unref(b);
    spin_unlock(&p->as->lock);
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
    for (u32 i = 0; i < PCI_SHM_COUNT; i++) {
        info.shm[i].offset  = k->shm[i].offset;
        info.shm[i].length  = k->shm[i].length;
        info.shm[i].bar     = k->shm[i].bar;
        info.shm[i].present = k->shm[i].present ? 1u : 0u;
        info.shm[i].shmid   = k->shm[i].shmid;
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
// Length is capped at SYS_RW_MAX per call (128 KiB since CF-3 A; ops
// above SYS_RW_STACK take a transient kmalloc bounce, smaller ops stay
// on the 4 KiB stack scratch). Userspace loops for larger transfers.
//
// Rights gate: SYS_READ requires RIGHT_READ on the handle; SYS_WRITE
// requires RIGHT_WRITE.

// CF-3 A audit F1: the per-Proc bounce budget (an I-32-shaped resource
// axis; PROC_BOUNCE_MAX, proc.h). The byte-I/O heap tier is TRANSIENT
// kernel memory a Proc can hold across an indefinitely-blocking
// dev->read/write (a held-open pipe, an idle /net socket, a hung server)
// -- unbudgeted, threads x SYS_RW_MAX of order-5 heap per Proc, fork-
// aggregable and buddy-fragmenting. Charge before the kmalloc, uncharge
// at the free on every path; over-budget ops degrade to the stack tier
// (a short op -- correct, never failed). PRINCIPAL_SYSTEM is exempt (the
// TCB pattern shared with page/thread/child caps); charge and uncharge
// gate on the same predicate, so the counter stays balanced.
static bool sys_bounce_charge(struct Proc *p, u64 n) {
    if (proc_resource_exempt(p)) return true;
    u64 cur = __atomic_load_n(&p->bounce_bytes, __ATOMIC_RELAXED);
    do {
        if (cur + n > (u64)PROC_BOUNCE_MAX) return false;
    } while (!__atomic_compare_exchange_n(&p->bounce_bytes, &cur, cur + n,
                                          false, __ATOMIC_RELAXED,
                                          __ATOMIC_RELAXED));
    return true;
}

static void sys_bounce_uncharge(struct Proc *p, u64 n) {
    if (proc_resource_exempt(p)) return;
    __atomic_fetch_sub(&p->bounce_bytes, n, __ATOMIC_RELAXED);
}

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

// Shared body behind SYS_WRITE (cursor) and SYS_PWRITE (positioned) -- #37.
// positioned=false reads the per-Spoor cursor and advances it by the accepted
// count, byte-identical to the pre-#37 sys_write_for_proc. positioned=true
// writes at the caller's absolute `off` and NEVER reads or advances the
// cursor -- the POSIX pwrite contract: concurrent positioned ops on one fd
// share no mutable state. The positioned arm adds three gates: off >= 0, no
// s64 overflow at off + len, and dev->seekable (the SYS_LSEEK gate, RW-4
// R2-F2) so positioned I/O on a pipe/cons/srv stream fails up front (the
// POSIX ESPIPE shape) instead of silently acting as a cursor-free write.
//
// Only KOBJ_SPOOR is writable (sys_lookup_rw_handle filters): the write
// routes through the Dev `.write` vtable, whose offset parameter has always
// been explicit (the Plan 9 shape) -- the cursor is syscall-layer sugar. A
// byte-mode /srv connection endpoint is itself a KOBJ_SPOOR conn Spoor, so
// its bytes ride this path too -- devsrv_write picks the server arm
// (srvconn_server_send_blocking, #348) or the CSRVCLIENT client arm
// (srvconn_client_send_blocking, CF-3 B) by the conn direction. The
// client-side KObj_Srv conn handle that once
// routed here was retired with SYS_SRV_CONNECT (stalk-3c); the
// kernel-attached no-direct-I/O guard moved with it into devsrv_write.
static s64 spoor_write_common(struct Proc *p, hidx_t h, const u8 *kbuf,
                              u64 len, bool positioned, s64 off) {
    // #100 (ER-3): the local rejects name their reason. `!p`/`!kbuf` stay a flat
    // -1 per the ERRORS.md preamble-guard rule -- they are internal-invariant
    // violations reachable only from a kernel caller (the EL0 handler always
    // passes its own scratch buffer), not caller errors.
    if (!p || (!kbuf && len > 0))                    return -1;
    if (positioned && off < 0)                       return -T_E_INVAL;
    // #844: c is a REF-HELD Spoor (the lookup transferred the ref); it keeps c
    // alive across the blocking dev->write even if a sibling closes the fd.
    // spoor_clunk on EVERY exit after the lookup.
    struct Spoor *c = sys_lookup_rw_handle(p, h, RIGHT_WRITE);
    // The lookup folds three rejects into one NULL -- no such handle, wrong kobj
    // kind, missing RIGHT_WRITE. All three are EBADF in POSIX terms (a write to
    // a fd not open for writing is EBADF, not EACCES), so one code covers them.
    if (!c)                                          return -T_E_BADF;
    // #81: a T_OPATH navigation handle is NOT a byte-I/O channel (it is born R|W
    // for create/walk-target use but perm_check-exempt at open). Reject every
    // write, including len 0, so it cannot serve content (IDENTITY-DESIGN 9.4 #81).
    // Linux answers EBADF for read/write on an O_PATH descriptor; so do we.
    if (c->flag & CWALKONLY)                       { spoor_clunk(c); return -T_E_BADF; }
    // ESPIPE is the POSIX answer here, but T_E_SPIPE (29) is not in the errno
    // registry and appending one is signoff-bearing (CLAUDE.md: ERRORS.md is
    // ABI-bearing). Left at the flat -1 rather than answering a plausible-but-
    // wrong EINVAL -- status quo, not a new wrongness. See #100's residual note.
    if (positioned && (!c->dev || !c->dev->seekable)) { spoor_clunk(c); return -1; }
    if (len == 0)                                  { spoor_clunk(c); return 0; }
    if (!c->dev || !c->dev->write)                 { spoor_clunk(c); return -T_E_INVAL; }
    if (positioned && len > (u64)INT64_MAX - (u64)off) { spoor_clunk(c); return -T_E_INVAL; }
    long n = c->dev->write(c, kbuf, (long)len, positioned ? off : c->offset);
    // #3 (Area F errno-rollout): propagate a Dev's real -errno (dev9p now
    // returns -T_E_* for an ecode in 2..4095) instead of collapsing to -1.
    // The legacy -1 sentinel is unchanged -- the pouch/native boundary decodes
    // it to EIO, NOT EPERM (errno.h forbids a handler returning -T_E_PERM=1);
    // an ecode==1/EPERM server error still collides with the -1 sentinel ->
    // EIO (a wider channel is the ER-rollout's job). Clamp an out-of-window
    // negative so a future Dev cannot punch a fake-huge "success" through
    // pouch's [-4095,-1] error window (symmetric with the native saturation).
    if (n < 0) {
        spoor_clunk(c);
        return (n < -4095) ? (s64)(-T_E_IO) : (s64)n;
    }
    if (!positioned) c->offset += n;
    spoor_clunk(c);
    return (s64)n;
}

// Inner — testable with kernel-side buf. Returns bytes written (>=0)
// or -1 on bad handle / wrong kind / missing rights / dev error.
s64 sys_write_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf, u64 len) {
    return spoor_write_common(p, h, kbuf, len, /*positioned=*/false, 0);
}

// SYS_PWRITE inner (#37) — testable with kernel-side buf. The cursor is
// untouched on every path.
s64 sys_pwrite_for_proc(struct Proc *p, hidx_t h, const u8 *kbuf, u64 len,
                        s64 off) {
    return spoor_write_common(p, h, kbuf, len, /*positioned=*/true, off);
}

// Shared body behind SYS_READ (cursor) and SYS_PREAD (positioned) -- #37.
// The read twin of spoor_write_common; see its comment for the positioned
// gates (off >= 0, off + len overflow, dev->seekable) and the cursor
// contract. positioned=false is byte-identical to the pre-#37
// sys_read_for_proc.
//
// Only KOBJ_SPOOR is readable (sys_lookup_rw_handle filters): the read
// routes through the Dev `.read` vtable. A byte-mode /srv connection
// endpoint is itself a KOBJ_SPOOR conn Spoor -- devsrv_read picks the server
// arm (srvconn_server_recv*) or the CSRVCLIENT client arm
// (srvconn_client_recv) by the conn direction. The client-side KObj_Srv conn
// handle that once routed here was retired with SYS_SRV_CONNECT (stalk-3c).
static s64 spoor_read_common(struct Proc *p, hidx_t h, u8 *kbuf, u64 len,
                             bool positioned, s64 off) {
    // #100 (ER-3): see the spoor_write_common twin for the disposition of each
    // reject, including why the non-seekable arm stays a flat -1.
    if (!p || (!kbuf && len > 0))                    return -1;
    if (positioned && off < 0)                       return -T_E_INVAL;
    // #844: c is a REF-HELD Spoor; it stays alive across the blocking
    // dev->read even if a sibling closes the fd. spoor_clunk on EVERY exit.
    struct Spoor *c = sys_lookup_rw_handle(p, h, RIGHT_READ);
    if (!c)                                          return -T_E_BADF;
    // #81: a T_OPATH navigation handle is NOT a byte-I/O channel -- reject every
    // read (the perm_check-exempt O_PATH open would otherwise be a read-bypass,
    // e.g. the 0400 /system.key via /bin/system.key). IDENTITY-DESIGN 9.4 #81.
    if (c->flag & CWALKONLY)                       { spoor_clunk(c); return -T_E_BADF; }
    if (positioned && (!c->dev || !c->dev->seekable)) { spoor_clunk(c); return -1; }
    if (len == 0)                                  { spoor_clunk(c); return 0; }
    if (!c->dev || !c->dev->read)                  { spoor_clunk(c); return -T_E_INVAL; }
    if (positioned && len > (u64)INT64_MAX - (u64)off) { spoor_clunk(c); return -T_E_INVAL; }
    long n = c->dev->read(c, kbuf, (long)len, positioned ? off : c->offset);
    // #3 (Area F errno-rollout): propagate a Dev's real -errno (dev9p now
    // returns -T_E_*) instead of collapsing to -1; clamp an out-of-window
    // negative to keep pouch's [-4095,-1] error window safe (see the write twin).
    if (n < 0) {
        spoor_clunk(c);
        return (n < -4095) ? (s64)(-T_E_IO) : (s64)n;
    }
    if (!positioned) c->offset += n;
    spoor_clunk(c);
    return (s64)n;
}

// Inner — testable with kernel-side buf. Returns bytes read (>=0; 0
// on EOF), -1 on bad handle / wrong kind / missing rights, or the Dev's
// negative -errno on a dev error (#3 -- dev9p now surfaces -T_E_IO etc.).
s64 sys_read_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len) {
    return spoor_read_common(p, h, kbuf, len, /*positioned=*/false, 0);
}

// SYS_PREAD inner (#37) — testable with kernel-side buf. The cursor is
// untouched on every path.
s64 sys_pread_for_proc(struct Proc *p, hidx_t h, u8 *kbuf, u64 len, s64 off) {
    return spoor_read_common(p, h, kbuf, len, /*positioned=*/true, off);
}

// Weft-6b-2 data drive: the zero-copy write fast-path. A large write whose user
// buffer points INTO a weft-bound /net data fd's shared ring moves through the
// ring (Tweftio) with NO copy-in -- so it is NOT capped at SYS_RW_MAX (the
// byte-copy scratch bound); the ring's payload region is the bound. Resolves the
// handle once; on a non-weft write it releases the ref and returns
// *handled = false so the caller takes the byte-copy path. Gated by the caller
// on len >= WEFT_HYBRID_THRESHOLD, so small writes never pay this lookup.
static s64 sys_write_weft_fastpath(struct Proc *p, hidx_t h, u64 buf_va,
                                   u64 len, bool *handled) {
    *handled = false;
    if (len > 0xFFFFFFFFull) return 0;              // beyond the u32 descriptor domain
    struct Spoor *c = sys_lookup_rw_handle(p, h, RIGHT_WRITE);
    if (!c) return 0;                               // bad fd -> the byte-copy path -EBADFs
    if (c->flag & CWALKONLY) { spoor_clunk(c); return 0; }   // O_PATH -> byte-copy rejects
    u32 accepted = 0;
    int v = dev9p_weft_try_write(c, buf_va, (u32)len, &accepted);
    if (v == 0) { spoor_clunk(c); return 0; }       // not a weft write -> byte-copy
    // v == 1 (handled OK) or v == -1 (weft transport error -- the flow is dead,
    // the byte-copy path would fail identically, so surface it).
    s64 r;
    if (v == 1) { c->offset += accepted; r = (s64)accepted; }
    else        { r = -1; }
    spoor_clunk(c);
    *handled = true;
    return r;
}

static s64 sys_write_handler(u64 hraw, u64 buf_va, u64 len) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -T_E_FAULT;   // #100

    // Weft zero-copy fast-path: a large write whose buffer points into a
    // weft-bound /net data fd's shared ring goes through the ring (no copy-in,
    // NOT SYS_RW_MAX-capped). Gated on the hybrid threshold so small writes are
    // unaffected (they fall straight through to the byte-copy path).
    if (len >= WEFT_HYBRID_THRESHOLD) {
        bool weft_handled = false;
        s64 wr = sys_write_weft_fastpath(p, (hidx_t)hraw, buf_va, len, &weft_handled);
        if (weft_handled) return wr;
    }

    if (len > SYS_RW_MAX) len = SYS_RW_MAX;

    if (len == 0) {
        // Validate the handle even for zero-length writes (POSIX
        // discipline: bad fd should return -EBADF regardless of len).
        // #100 (ER-3): it now DOES -- this comment stated the intent while the
        // code below returned the flat -1 that reads as EPERM to a Linux guest.
        // #844: the lookup transfers a Spoor ref; release it immediately
        // (validation only -- no I/O on a zero-length write).
        struct Spoor *c0 = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_WRITE);
        if (!c0)                                     return -T_E_BADF;
        // #81 F1: the gate must cover the len==0 fast-path too (it short-circuits
        // before sys_write_for_proc) -- an O_PATH handle does NO byte I/O, incl. 0.
        if (c0->flag & CWALKONLY)                  { spoor_clunk(c0); return -T_E_BADF; }
        spoor_clunk(c0);
        return 0;
    }

    // CF-3 A: two-tier bounce. Ops <= SYS_RW_STACK stay on the stack (the
    // metadata-storm path -- zero new cost; 4 KiB frame vs the 16 KiB kernel
    // stack); bulk ops take a transient kmalloc so ONE syscall stages up to
    // SYS_RW_MAX. kmalloc failure degrades to the stack tier -- memory
    // pressure shortens a write (POSIX short writes are normal), never
    // fails it. uaccess_copy_in replaces the per-byte load loop.
    u8 stack_scratch[SYS_RW_STACK];
    u8 *scratch = stack_scratch;
    void *heap_scratch = NULL;
    if (len > SYS_RW_STACK) {
        if (sys_bounce_charge(p, len)) {
            heap_scratch = kmalloc(len, 0);
            if (heap_scratch) scratch = heap_scratch;
            else              sys_bounce_uncharge(p, len);
        }
        if (!heap_scratch) len = SYS_RW_STACK;
    }
    // #100 (ER-3): a copy-in fault is EFAULT. This is the arm that actually
    // fires on a genuinely unmapped page -- sys_validate_user_buf above is only
    // a range check, so fixing that one and leaving this is exactly the
    // "the fix on site N stops you asking about site N+1" shape.
    if (uaccess_copy_in(scratch, buf_va, len) != 0) {
        if (heap_scratch) { kfree(heap_scratch); sys_bounce_uncharge(p, len); }
        return -T_E_FAULT;
    }
    s64 wr = sys_write_for_proc(p, (hidx_t)hraw, scratch, len);
    if (heap_scratch) { kfree(heap_scratch); sys_bounce_uncharge(p, len); }
    return wr;
}

// Weft-6b-3 data drive (RX): the zero-copy read fast-path. A large read whose
// user buffer points INTO a weft-bound /net data fd's shared ring recvs through
// the ring (Tweftio READ) with NO copy-out -- netd writes the bytes directly into
// the guest's shared mapping, so the read is NOT capped at SYS_RW_MAX (the
// byte-copy scratch bound) and the handler does NO uaccess_store on this path.
// Resolves the handle once; on a non-weft read it releases the ref and returns
// *handled = false so the caller takes the byte-copy path. Gated by the caller on
// len >= WEFT_HYBRID_THRESHOLD, so small reads never pay this lookup. Mirrors
// sys_write_weft_fastpath.
static s64 sys_read_weft_fastpath(struct Proc *p, hidx_t h, u64 buf_va,
                                  u64 len, bool *handled) {
    *handled = false;
    if (len > 0xFFFFFFFFull) return 0;              // beyond the u32 descriptor domain
    struct Spoor *c = sys_lookup_rw_handle(p, h, RIGHT_READ);
    if (!c) return 0;                               // bad fd -> the byte-copy path -EBADFs
    if (c->flag & CWALKONLY) { spoor_clunk(c); return 0; }   // O_PATH -> byte-copy rejects
    u32 got = 0;
    int v = dev9p_weft_try_read(c, buf_va, (u32)len, &got);
    if (v == 0) { spoor_clunk(c); return 0; }       // not a weft read -> byte-copy
    // v == 1 (handled OK; netd wrote the bytes into the guest's ring) or v == -1
    // (weft transport error -- the flow is dead, the byte-copy path would fail
    // identically, so surface it).
    s64 r;
    if (v == 1) { c->offset += got; r = (s64)got; }
    else        { r = -1; }
    spoor_clunk(c);
    *handled = true;
    return r;
}

static s64 sys_read_handler(u64 hraw, u64 buf_va, u64 len) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -T_E_FAULT;   // #100

    // Weft zero-copy fast-path (RX): a large read whose buffer points into a
    // weft-bound /net data fd's shared ring recvs through the ring (no copy-out,
    // NOT SYS_RW_MAX-capped). Gated on the hybrid threshold so small reads are
    // unaffected (they fall straight through to the byte-copy path).
    if (len >= WEFT_HYBRID_THRESHOLD) {
        bool weft_handled = false;
        s64 rd = sys_read_weft_fastpath(p, (hidx_t)hraw, buf_va, len, &weft_handled);
        if (weft_handled) return rd;
    }

    if (len > SYS_RW_MAX) len = SYS_RW_MAX;

    if (len == 0) {
        struct Spoor *c0 = sys_lookup_rw_handle(p, (hidx_t)hraw, RIGHT_READ);
        if (!c0)                              return -T_E_BADF;   // #844: validate + release
        // #81 F1: the gate must cover the len==0 fast-path too (it short-circuits
        // before sys_read_for_proc) -- an O_PATH handle does NO byte I/O, incl. 0.
        if (c0->flag & CWALKONLY)                  { spoor_clunk(c0); return -T_E_BADF; }
        spoor_clunk(c0);
        return 0;
    }

    // CF-3 A: two-tier bounce (see sys_write_handler). Pre-CF-3 the 4 KiB
    // stack scratch capped every bulk read RPC at 4 KiB against a 32 KiB
    // negotiated msize -- 67% of a go build's Treads were exactly-4096
    // userspace chunks (the CF3 Tread-stream measurement).
    u8 stack_scratch[SYS_RW_STACK];
    u8 *scratch = stack_scratch;
    void *heap_scratch = NULL;
    if (len > SYS_RW_STACK) {
        if (sys_bounce_charge(p, len)) {
            heap_scratch = kmalloc(len, 0);
            if (heap_scratch) scratch = heap_scratch;
            else              sys_bounce_uncharge(p, len);
        }
        if (!heap_scratch) len = SYS_RW_STACK;
    }
    s64 got = sys_read_for_proc(p, (hidx_t)hraw, scratch, len);
    // Bulk copy-out; on fault, return -1 — partial bytes already in
    // user-VA are not "uncopied" but bytes consumed beyond the fault are
    // LOST. Documented caveat (unchanged from the per-byte era).
    if (got > 0 && uaccess_copy_out(buf_va, scratch, (u64)got) != 0)
        got = -T_E_FAULT;   // #100 (ER-3): see the copy-in twin
    if (heap_scratch) { kfree(heap_scratch); sys_bounce_uncharge(p, len); }
    return got;
}

// =============================================================================
// SYS_PREAD / SYS_PWRITE — positioned byte I/O (#37).
// =============================================================================
//
// Thin twins of sys_read_handler / sys_write_handler: the same user-buffer
// validation + SYS_RW_MAX clamp + per-byte uaccess staging, routed to the
// positioned inners. Deliberately NO weft fast-path -- a weft flow is a
// stream, so positioned I/O on it has no meaning; the byte path's
// dev->seekable gate rejects nothing there (dev9p is seekable) but netd's
// data files are the only weft-bound fids and no consumer preads them, while
// wiring the fast-path would put the cursor-free contract inside the
// weft accounting for zero benefit. len==0 rides the inner's
// validate-then-0 path (the POSIX EBADF discipline incl. the #81 O_PATH
// gate and the positioned off/seekable gates).

static s64 sys_pread_handler(u64 hraw, u64 buf_va, u64 len, u64 off_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -1;
    if (len > SYS_RW_MAX) len = SYS_RW_MAX;

    // CF-3 A: two-tier bounce (see sys_read_handler).
    u8 stack_scratch[SYS_RW_STACK];
    u8 *scratch = stack_scratch;
    void *heap_scratch = NULL;
    if (len > SYS_RW_STACK) {
        if (sys_bounce_charge(p, len)) {
            heap_scratch = kmalloc(len, 0);
            if (heap_scratch) scratch = heap_scratch;
            else              sys_bounce_uncharge(p, len);
        }
        if (!heap_scratch) len = SYS_RW_STACK;
    }
    s64 got = sys_pread_for_proc(p, (hidx_t)hraw, scratch, len, (s64)off_raw);
    // Bulk copy-out; the SYS_READ partial-copy caveat applies -- but unlike
    // SYS_READ nothing is LOST on a fault: the cursor never moved, so the
    // caller can simply repeat the pread.
    if (got > 0 && uaccess_copy_out(buf_va, scratch, (u64)got) != 0)
        got = -T_E_FAULT;   // #100 (ER-3): see the copy-in twin
    if (heap_scratch) { kfree(heap_scratch); sys_bounce_uncharge(p, len); }
    return got;
}

static s64 sys_pwrite_handler(u64 hraw, u64 buf_va, u64 len, u64 off_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    if (!sys_validate_user_buf(buf_va, len))         return -1;
    if (len > SYS_RW_MAX) len = SYS_RW_MAX;

    // CF-3 A: two-tier bounce (see sys_write_handler).
    u8 stack_scratch[SYS_RW_STACK];
    u8 *scratch = stack_scratch;
    void *heap_scratch = NULL;
    if (len > SYS_RW_STACK) {
        if (sys_bounce_charge(p, len)) {
            heap_scratch = kmalloc(len, 0);
            if (heap_scratch) scratch = heap_scratch;
            else              sys_bounce_uncharge(p, len);
        }
        if (!heap_scratch) len = SYS_RW_STACK;
    }
    // #100 (ER-3): a copy-in fault is EFAULT. This is the arm that actually
    // fires on a genuinely unmapped page -- sys_validate_user_buf above is only
    // a range check, so fixing that one and leaving this is exactly the
    // "the fix on site N stops you asking about site N+1" shape.
    if (uaccess_copy_in(scratch, buf_va, len) != 0) {
        if (heap_scratch) { kfree(heap_scratch); sys_bounce_uncharge(p, len); }
        return -T_E_FAULT;
    }
    s64 wr = sys_pwrite_for_proc(p, (hidx_t)hraw, scratch, len, (s64)off_raw);
    if (heap_scratch) { kfree(heap_scratch); sys_bounce_uncharge(p, len); }
    return wr;
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
    // #100 (ER-3): handle_close's own -1 means "no such slot / not a live
    // handle" -- EBADF, the only failure close(2) has. Mapped HERE rather than
    // inside handle_close so the internal contract (and its ~20 kernel callers,
    // which test == 0 or ignore the result) stays byte-unchanged.
    return handle_close(p, (hidx_t)hraw) == 0 ? 0 : (s64)(-T_E_BADF);
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
// SYS_FSTAT routes through dev->stat_native; six Devs implement it today
// (devramfs, dev9p, devhw, devsrv, devproc, devpci -- #46 audit F3 refresh
// of the stale "only devramfs" 16b-gamma note). Devs without it leave the
// slot NULL and fstat returns -1, the graceful "no stat for this kind of
// object" answer. KOBJ_SRV handles are rejected at the kind gate; a devsrv
// conn SPOOR (dc='s') serves fstat via devsrv_stat_native since #957.
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
    int rc = c->dev->stat_native(c, out);
    // #100: the device identity is the Spoor's (Plan 9 Chan.dev), not the Dev's.
    // Stamp it after a clean fill so (devno, qid.path) uniquely names a file across
    // mount sessions -- fstat and the stat leaf-Spoor fallback both land here; the
    // pounce fused-query path stamps its own leaf in stalk_core. Static single-
    // instance Devs carry devno 0 (unchanged); dev9p sessions carry a live value.
    if (rc == 0) out->devno = c->devno;
    return rc;
}

// #194: sample the backing file's size for the Image-cache file_limit stamp.
// BURROW_FILE_LIMIT_UNKNOWN when the Dev cannot answer -- the CALLER owns the
// failure policy (exec admits unknown for the immutable baked ramfs; the
// guest-facing phenotype mmap arm refuses it).
u64 spoor_file_size(struct Spoor *c) {
    struct t_stat st;
    if (spoor_stat_native(c, &st) != 0)
        return BURROW_FILE_LIMIT_UNKNOWN;
    return st.size;
}

// Inner — kernel-side core (the #37 *_for_proc testable shape): resolve `hraw`
// in `p`'s handle table and fill a KERNEL `struct t_stat`. Extracted at V-1b so
// the native handler and the VIVARIUM `fstat` translator share ONE body: the
// kind gate, the ref discipline and the Dev call are literally the same code
// for a phenotyped and a native caller, which is how I-43 ("shape, never
// authority") holds by construction rather than by review.
static s64 sys_fstat_for_proc(struct Proc *p, u64 hraw, struct t_stat *out_k) {
    if (!p || !out_k)                                return -1;

    // No rights mask (#46): fstat observes metadata, not content -- POSIX
    // fstat(2) works on ANY valid fd (Linux: O_WRONLY, O_PATH, anything;
    // Plan 9 Tstat / 9P2000.L Tgetattr have no open/read requirement). The
    // original RIGHT_READ tightening ("every v1.0 caller that fstats also
    // reads") was falsified by the standard POSIX write-then-stat pattern:
    // an O_WRONLY create mints a WRITE-only handle (omode-derived rights,
    // A-3 F1), and cmd/go's putIndexEntry fstats exactly such an fd for its
    // truncate no-op gate -- the -1 made it self-delete every fresh go-cache
    // index entry. The tightening also guarded nothing for any Proc that
    // can WALK the path: the same file's metadata is already reachable by
    // re-walking it O_PATH (#81 keeps fstat allowed on O_PATH, the Linux
    // semantics). The one real residual (#46 audit F1) -- a spawn-endowed
    // rights-stripped handle in a child whose Territory cannot walk the
    // file now reveals its metadata -- is ACCEPTED by the POSIX/Plan 9
    // fd-passing precedent (a passed fd conveys fstat; the endower chose to
    // pass it; rights-stripping bounds read/write/transfer, never metadata
    // secrecy). Kind-gate only (KOBJ_SPOOR; rejects KOBJ_SRV) -- the
    // SYS_LSEEK rights-0 precedent. #844: c is REF-HELD; spoor_clunk on
    // every exit -- the ref keeps c alive across the (possibly blocking)
    // dev->stat_native.
    struct Spoor *c = sys_lookup_rw_handle(p, (hidx_t)hraw, 0);
    if (!c)                                           return -1;

    // Fill a kernel-scratch t_stat from the Dev. Failing the Dev's
    // stat_native (NULL slot or error return) maps to -1 here.
    if (spoor_stat_native(c, out_k) != 0)           { spoor_clunk(c); return -1; }
    spoor_clunk(c);
    return 0;
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

    struct t_stat ks;
    s64 rc = sys_fstat_for_proc(p, hraw, &ks);
    if (rc != 0)                                     return rc;

    // Copy out to user-VA. Per-byte uaccess_store_u8; on fault the user-VA may
    // have partially-written bytes (consistent with SYS_READ).
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
    // structurally invalid calls. #100 (ER-3): EINVAL, POSIX's answer for an
    // unrecognized whence.
    //
    // #102 F6: whence is a 32-bit selector, so only the low half of x2 is
    // significant. POSIX declares it `int` — which this comment already appeals
    // to — and fd, the far more dangerous argument, is ALREADY narrowed the same
    // way three lines down (`(hidx_t)hraw`, and hidx_t is int). The full-width
    // compare was an artifact of the raw argument arriving as u64, not a
    // designed strictness: nothing rests on the high bits of an enum, and
    // libthyla-rs already declares the parameter u32.
    //
    // It matters because of who else reaches here. VIVARIUM's T1 table renumbers
    // Linux lseek onto this handler and copies the argument words VERBATIM
    // (vivarium.c, viv_linux_dispatch), and Linux's own SYSCALL_DEFINE narrows
    // `unsigned int whence` by construction — so a guest that leaves junk in
    // x2[63:32] gets EINVAL here for a seek Linux would have performed. Every
    // pure translator in vivarium.c narrows its int arguments for this reason;
    // this is the one place the T1 row's "identical in width" claim was
    // enforced by caller convention rather than by code.
    //
    // Narrowing costs nothing: a bad low half (3, or -1 == 0xFFFFFFFF) still
    // fails the range check below, exactly as Linux's does.
    u32 whence = (u32)whence_raw;
    if (whence != T_SEEK_SET &&
        whence != T_SEEK_CUR &&
        whence != T_SEEK_END)                        return -T_E_INVAL;

    // No rights mask: lseek manipulates the per-Spoor cursor, not content.
    // #844: c is REF-HELD (sys_lookup_rw_handle kind-gates to KOBJ_SPOOR +
    // RIGHT 0); the ref keeps c alive across the SEEK_END dev->stat_native
    // (which may block). spoor_clunk on EVERY exit below.
    struct Spoor *c = sys_lookup_rw_handle(p, (hidx_t)hraw, 0);
    if (!c)                                           return -T_E_BADF;   // #100

    // Reject lseek on a non-seekable Dev (POSIX lseek(2) on a pipe -> ESPIPE).
    // RW-4 R2-F2: the old `dev->stat_native == NULL` heuristic broke when #957
    // (devsrv) + A-4b (devproc) gave non-seekable Devs a .stat_native for fstat,
    // regressing lseek to succeed on an offset their read/write ignore. The
    // explicit dev->seekable flag (devramfs + dev9p only) decouples fstat-ability
    // from seekability.
    //
    // #100 (ER-3) RESIDUAL: ESPIPE is what POSIX names, and it is precisely
    // what this arm means -- but T_E_SPIPE (29) is not in the registry, and
    // appending an errno is signoff-bearing (CLAUDE.md: ERRORS.md is ABI-bearing,
    // updates require user signoff). Answering EINVAL instead would be the
    // "differently wrong" trap #100's own analysis warns about, so this stays
    // at the flat -1 until the append is approved. The three sibling sites are
    // the positioned arms of spoor_{read,write}_common.
    if (!c->dev->seekable)                          { spoor_clunk(c); return -1; }

    s64 offset = (s64)offset_raw;
    s64 new_off;

    // #100 (ER-3): every arm below rejects because the RESULTING OFFSET is
    // unrepresentable or negative, which is exactly what POSIX lseek(2) names
    // EINVAL for -- so unlike the ESPIPE arm above, there is a correct code
    // available and no guessing involved. (EOVERFLOW would be the finer answer
    // for the two pure-overflow guards, but T_E_OVERFLOW is not in the registry
    // and the outcome is the same class: no valid offset exists.) The two
    // stat-failure arms are EIO -- the size could not be determined.
    switch (whence) {
    case T_SEEK_SET:
        if (offset < 0)                             { spoor_clunk(c); return -T_E_INVAL; }
        new_off = offset;
        break;
    case T_SEEK_CUR: {
        s64 cur = c->offset;
        if (offset > 0 && cur > INT64_MAX - offset) { spoor_clunk(c); return -T_E_INVAL; }
        if (offset < 0 && cur < INT64_MIN - offset) { spoor_clunk(c); return -T_E_INVAL; }
        new_off = cur + offset;
        if (new_off < 0)                            { spoor_clunk(c); return -T_E_INVAL; }
        break;
    }
    case T_SEEK_END: {
        struct t_stat ks;
        if (spoor_stat_native(c, &ks) != 0)         { spoor_clunk(c); return -T_E_IO; }
        s64 size = (s64)ks.size;
        if (size < 0)                               { spoor_clunk(c); return -T_E_IO; }
        if (offset > 0 && size > INT64_MAX - offset){ spoor_clunk(c); return -T_E_INVAL; }
        if (offset < 0 && size < INT64_MIN - offset){ spoor_clunk(c); return -T_E_INVAL; }
        new_off = size + offset;
        if (new_off < 0)                            { spoor_clunk(c); return -T_E_INVAL; }
        break;
    }
    default:
        // Unreachable: whence was range-checked above. Kept as a fail-closed
        // backstop, and EINVAL for the same reason the range check is.
        spoor_clunk(c);
        return -T_E_INVAL;
    }

    c->offset = new_off;
    spoor_clunk(c);
    return new_off;
}

// =============================================================================
// SYS_STAT — path-stat in one syscall (POUNCE; docs/POUNCE-DESIGN.md §7).
// =============================================================================
//
// Replaces the O_PATH walk-open + SYS_FSTAT + close emulation (3 syscalls,
// 13 RPCs on a 4-deep dev9p path) with one syscall whose resolution is the
// stalk walk-QUERY: on a walk_attrs Dev the leaf's attrs arrive fused with
// the walk — 1 RPC, no handle, no Spoor, no server fid. The path X-search is
// byte-identical to the emulation's (STALK_STAT == STALK_WALK's checks;
// POSIX stat authority is the path X-search only, which is exactly what
// O_PATH granted: it skips the R/W perm_check, and SYS_FSTAT is kind-gated
// only, #46).

// Inner — kernel-side core (the #37 *_for_proc testable shape): `path` is
// kernel memory, NUL-terminated at path[path_len], already NUL-free within;
// *out_k is kernel scratch. Returns 0 / -1 (structural) / -errno (resolution).
// D-1: `stalk_flags` is 0 (follow a final symlink -- POSIX stat) or
// STALK_NOFOLLOW (the lstat shape -- the final link's OWN record); the
// vivarium newfstatat shell passes it for AT_SYMLINK_NOFOLLOW. Native SYS_STAT
// always passes 0 (no native lstat surface at D-1 -- a recorded seam).
s64 sys_stat_for_proc(struct Proc *p, const char *path, u64 path_len,
                      u32 stalk_flags, struct t_stat *out_k) {
    if (!p || !path || !out_k)                       return -1;
    if (path_len == 0 || path_len > SYS_OPEN_PATH_MAX) return -1;
    if (!p->territory)                               return -1;

    // RW-4 SA-F1: atomic root read+ref under ns_lock (races a concurrent
    // pivot_root otherwise).
    struct Spoor *start = territory_root_ref(p->territory);
    if (!start)                                      return -1;

    // LS-4: a relative path resolves against the Territory cwd — the same
    // join SYS_OPEN's FROM_ROOT arm performs (stalk re-clamps '..' at
    // root_spoor, so the join cannot escape containment; I-28). #83: the join
    // is verbatim, so stalk applies its own '.'/'..'/trailing-slash gates.
    char joined[SYS_OPEN_PATH_MAX + 1];
    const char *rpath = path;
    u64 rlen = path_len;
    if (path[0] != '/') {
        int jl = territory_join_cwd(p->territory, path, path_len,
                                    joined, sizeof(joined));
        if (jl < 0) { spoor_clunk(start); return -1; }
        rpath = joined;
        rlen  = (u64)jl;
    }

    int serr = T_E_NOENT;
    int rc = stalk_stat(p, start, rpath, rlen, stalk_flags, out_k, &serr);
    spoor_clunk(start);
    if (rc != 0)                                     return -(s64)serr;
    return 0;
}

static s64 sys_stat_handler(u64 path_va, u64 path_len_raw, u64 stat_va) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if (path_len_raw == 0)                           return -1;
    if (path_len_raw > SYS_OPEN_PATH_MAX)            return -1;
    if (!sys_validate_user_buf(path_va, path_len_raw)) return -1;
    if (!sys_validate_user_buf(stat_va, sizeof(struct t_stat))) return -1;

    // Copy the path into kernel scratch + reject embedded NUL (the SYS_OPEN
    // prologue shape; '/' is allowed — stalk tokenizes it).
    char path_scratch[SYS_OPEN_PATH_MAX + 1];
    for (u64 i = 0; i < path_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(path_va + i, &b) != 0)   return -1;
        if (b == '\0')                               return -1;
        path_scratch[i] = (char)b;
    }
    path_scratch[path_len_raw] = '\0';

    struct t_stat ks;
    s64 rc = sys_stat_for_proc(p, path_scratch, path_len_raw, 0, &ks);
    if (rc != 0)                                     return rc;

    // Copy out to user-VA (per-byte, the SYS_FSTAT shape; a fault may leave
    // partially-written bytes, consistent with SYS_READ).
    const u8 *src = (const u8 *)&ks;
    for (u64 i = 0; i < sizeof(struct t_stat); i++) {
        if (uaccess_store_u8(stat_va + i, src[i]) != 0) return -1;
    }
    return 0;
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
// 4 KiB matches PIPE_BUF_SIZE + SYS_RW_STACK; aligns with the design.
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
                                       u64 aname_len, u64 n_uname,
                                       u64 flags) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Validate aname length cap.
    if (aname_len > SYS_ATTACH_ANAME_MAX)             return -1;
    // n_uname is 9P2000.L's u32 numeric uid field; reject values that
    // would silently truncate to u32. Mirrors SYS_ATTACH_9P F239 fix.
    if (n_uname > (u64)0xFFFFFFFFu)                   return -1;
    // Reject unknown flag bits (forward-compat guard). SYS_ATTACH_9P_LOOSE
    // is the B1 per-attach I-38 opt-in (docs/chase/B1-VOTE.md + the ARCH
    // I-38 row): the mounter asserts the single-writer premise for this
    // attach; the minted client's cached-opens then serve full hint hits
    // without the per-open wire revalidation. The #112 ABI discipline:
    // this is arg x4 -- EVERY caller sets it (the lib wrappers take it
    // explicitly, so a stale caller cannot pass garbage silently).
    if (flags & ~(u64)SYS_ATTACH_9P_LOOSE)            return -1;
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
        cn, aname_len > 0 ? aname_scratch : NULL, aname_len, p->principal_id,
        (flags & SYS_ATTACH_9P_LOOSE) != 0, &aerr);
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

    // #80 (errno-rollout, ER-1): every failure below answers a SPECIFIC -T_E_*
    // rather than the flat -1. The flat sentinel is rendered EIO by the pouch
    // boundary-line and mapped to Error::Io by libthyla-rs, so before this a
    // permission denial, a bad fd, and a malformed argument were one
    // indistinguishable "I/O error" to every caller. The walk-miss arms were
    // already upgraded (they are the Go-build keystone); this finishes the
    // handler. The `!t` / `!p` preamble guards above stay -1 -- they are
    // structurally unreachable from EL0 (no current thread) and share that shape
    // with every other handler in this file.
    //
    // Blast radius: SYS_WALK_OPEN's consumers are libthyla-rs (fs::, via
    // file::with_parent_dir) and libt. pouch reached it until PTY-3, when patch
    // 0021 repointed openat onto SYS_OPEN -- so no pouch program observes these
    // returns today.

    // Validate name length cap. name_len_raw == 0 is rejected: a zero-
    // length name is a clone-walk (nname=0 in the 9P sense), which has
    // no userspace use case for an opened fd at v1.0 — the attach root
    // is already opened.
    if (name_len_raw == 0)                            return -T_E_INVAL;
    if (name_len_raw > SYS_WALK_OPEN_NAME_MAX)        return -T_E_INVAL;
    // A user buffer that does not validate is EFAULT, not EINVAL: the caller's
    // pointer is bad, its length is fine.
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -T_E_FAULT;

    // Validate omode bit set. Rejecting unknown bits lets future bits be
    // added without ambiguity (an old kernel rejects a new bit; a new
    // kernel accepts both old + new bits).
    if (omode_raw & ~(u64)SYS_WALK_OPEN_OMODE_VALID)  return -T_E_INVAL;

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
        // #80: "the caller has no root to walk from" is a state error, not a bad
        // fd -- no fd was named. A Proc reaches this only before its first
        // SYS_CHROOT.
        if (!p->territory)                            return -T_E_INVAL;
        // RW-4 SA-F1: atomic read+ref under the Territory ns_lock. The prior
        // read-then-spoor_ref left a UAF window: a concurrent pivot_root could
        // swap root_spoor + clunk the old one to zero between the read and the
        // ref. territory_root_ref closes it.
        src = territory_root_ref(p->territory);
        if (!src)                                     return -T_E_INVAL;
    } else {
        // #80: covers both "no such handle" and "the handle lacks RIGHT_READ" --
        // EBADF either way, which is what POSIX says about an fd that cannot
        // serve the requested operation.
        src = sys_lookup_spoor(p, (hidx_t)spoor_fd_raw, RIGHT_READ);   // ref-held
        if (!src)                                     return -T_E_BADF;
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
        if (stalk_cross_mounts(p, src, &crossed) < 0)    { spoor_clunk(src); return -T_E_IO; }
        if (crossed) { spoor_clunk(src); src = crossed; }
    }
    // #80: split what was one flat reject, so a caller learns which end was at
    // fault -- no walk slot means not a searchable directory (ENOTDIR, the #79
    // vocabulary); walkable but no open slot means the operation is simply
    // absent (OPNOTSUPP). Both arms are DEFENSIVE at v1.0: all 18 Devs fill
    // .walk, and a Spoor reaching here always carries a dev. The reachable
    // not-a-directory case is a walk slot that RETURNS NULL, which lands on the
    // walk-fail arm below.
    if (!src->dev || !src->dev->walk)                 { spoor_clunk(src); return -T_E_NOTDIR; }
    if (!src->dev->open)                              { spoor_clunk(src); return -T_E_OPNOTSUPP; }
    // #81: the single-hop twin of stalk's #79 gate -- the thing being searched
    // must BE a directory. The check above only proves the DEV has a walk slot;
    // a walkable Dev's FILE Spoor passes it.
    //
    // MEASURED pre-gate behaviour (not inferred): walking a name out of a 0644
    // file answered -T_E_ACCES, because the X-search below reached the file
    // first and denied on its missing x bit. A 0755 file would instead have
    // sailed past the X-search and reported the walk-miss as ENOENT. That is
    // exactly the #79 mode-dependence -- the same situation answering two
    // different errnos depending on a bit with no bearing on the question --
    // so the gate goes BEFORE the X-search, not after it.
    //
    // Placed after the mount cross above, so a mount point is judged by the
    // tree it actually resolves to.
    if (!(src->qid.type & QTDIR))                     { spoor_clunk(src); return -T_E_NOTDIR; }

    // A-2d (IDENTITY-DESIGN.md 3.7.1): search (X) permission on the source
    // directory before walking into it. Gated on the Dev's perm_enforced flag
    // (dev9p deferred to A-3; devramfs live). devramfs dirs are 0555, so the
    // PRINCIPAL_SYSTEM owner traverses while a non-system principal needs
    // other-x. fail-closed if the Dev cannot vouch for the metadata. This is
    // additive to the handle-RIGHT gate above (capability axis); both must hold.
    if (src->dev->perm_enforced) {
        struct t_stat src_st;
        if (spoor_stat_native(src, &src_st) != 0)        { spoor_clunk(src); return -T_E_IO; }
        // #80: a denied X-search is EACCES. errno.h forbids returning
        // -T_E_PERM (it collides with the flat sentinel), so ACCES is the
        // registry's permission-denied code -- the same one stalk_err writes.
        if (perm_check(p, &src_st, PERM_X) != 0)         { spoor_clunk(src); return -T_E_ACCES; }
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
        // #80: the load failed on the caller's page -> EFAULT; a byte the
        // component grammar forbids -> EINVAL.
        if (uaccess_load_u8(name_va + i, &b) != 0)    { spoor_clunk(src); return -T_E_FAULT; }
        if (b == '/' || b == '\0')                    { spoor_clunk(src); return -T_E_INVAL; }
        name_scratch[i] = (char)b;
    }
    if (name_len_raw == 1 && name_scratch[0] == '.')  { spoor_clunk(src); return -T_E_INVAL; }
    if (name_len_raw == 2 && name_scratch[0] == '.' &&
                              name_scratch[1] == '.') { spoor_clunk(src); return -T_E_INVAL; }
    // Unconditional NUL terminator — REQUIRED for dev9p_walk's strlen scan.
    name_scratch[name_len_raw] = '\0';

    // Clone the source Spoor — gives us an independent cursor whose aux
    // dev->walk will replace with a freshly-allocated priv (carrying the
    // new fid). The clone starts at ref=1; spoor_clunk on failure runs
    // dev->close (clunks the fid if walk had progressed) + drops the ref.
    struct Spoor *nc = spoor_clone(src);
    if (!nc)                                          { spoor_clunk(src); return -T_E_NOMEM; }

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
        return -T_E_NOENT;   // errno-rollout (ER-1): walk-miss -> NotFound
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
        // #80: a Dev violating the reuse-nc convention is a kernel-internal
        // contract breach, not anything the caller did -> EIO.
        return -T_E_IO;
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
        return -T_E_NOENT;   // errno-rollout (ER-1): walk-miss -> NotFound
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
        if (stalk_cross_mounts(p, nc, &crossed) < 0)  { spoor_clunk(nc); return -T_E_IO; }
        if (crossed) { spoor_clunk(nc); nc = crossed; }
    }

    // FS-delta (IDENTITY-DESIGN.md §9.4): SYS_WALK_OPEN_OPATH skips the
    // open. nc is walked (dev9p_walk set its fid + qid) but NOT Tlopen'd,
    // yielding a non-opened, walkable handle -- the valid base for
    // creating/walking children + a valid chroot target. 9P forbids Twalk
    // from an opened fid, so a normally-opened handle cannot serve that
    // role. The access bits are irrelevant for an O_PATH handle.
    // DISTRO D-1: the single-hop twin does not EXPAND a symlink (it is a walk
    // primitive, not a resolution -- one hop cannot follow a multi-component
    // target), but it must not hand one to Dev.open either. Two reasons, and
    // the second is why this is a gate and not a nicety:
    //
    //   1. The DAC check below would read the LINK's mode, and a symlink is
    //      minted 0777 by POSIX convention -- so `other` is rwx and the check
    //      passes for every principal regardless of who owns the link or its
    //      directory. The gate is vacuous on exactly this shape.
    //   2. What Dev.open then does is a SERVER property. Stratum's h_lopen
    //      rejects a symlink only under O_TRUNC, so a plain OREAD/OWRITE Tlopen
    //      on a link fid SUCCEEDS; the resulting writes land in the link's own
    //      inode (silent data loss, not a redirect). Against a path-based 9P
    //      server that implements Tlopen as open(path, flags), the same call
    //      opens the TARGET with only the link's 0777 checked -- a complete DAC
    //      bypass. I-14 forbids resting a kernel gate on server behaviour.
    //
    // So: with O_PATH the handle IS the link (the v1.0 lstat spelling, and the
    // base a caller needs to unlink or rename it); without it, T_E_LOOP --
    // matching stalk's quarry gate exactly, and giving SYS_WALK_OPEN_NOFOLLOW
    // its meaning here instead of admitting it as a silent no-op.
    if ((nc->qid.type & QTSYMLINK) && !(omode_raw & SYS_WALK_OPEN_OPATH)) {
        spoor_clunk(nc);
        return -T_E_LOOP;
    }

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
            if (spoor_stat_native(nc, &nc_st) != 0)  { spoor_clunk(nc); return -T_E_IO; }
            if (perm_check(p, &nc_st, perm_want_for_omode((u32)omode_raw)) != 0) {
                spoor_clunk(nc);
                return -T_E_ACCES;   // #80: the target denies this access mode
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
        // D-1: strip the resolution flag -- it is consumed by the gate above
        // and means nothing to a Dev (dev9p masks it off the wire anyway, but
        // it would still be STORED in Spoor.mode). SYS_OPEN strips it at its
        // own call site; the twins must not be the asymmetric ones.
        u32 omode_dev = (u32)(omode_raw & ~(u64)SYS_WALK_OPEN_NOFOLLOW);
        struct Spoor *opened = nc->dev->open(nc, (int)omode_dev);
        if (!opened) {
            spoor_clunk(nc);
            // #80 seam: Dev.open returns Spoor* with no errno channel -- the
            // same shape that forced #99's create_errno side-channel. Until it
            // grows one, a failed open is EIO. Reachable causes today are a
            // dev9p Tlopen refusal and a devsrv connect failure; the walk
            // already succeeded, so this is never "no such file".
            return -T_E_IO;
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
        // #80 seam: POSIX names a full per-process fd table EMFILE (24), which
        // is not in the errno registry (an ERRORS.md append needs signoff).
        // ENOMEM is the registered out-of-resources code and is at least true.
        return -T_E_NOMEM;
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

    // #83: SYS_CHDIR is the one caller that needs BOTH cwd jobs, and it needs
    // them in this order.
    //
    // (1) JOIN verbatim, for the physical check. "."/".."/a trailing separator
    // survive so stalk gates them -- so `cd f/..` where f is a FILE, or
    // `cd nonexistent/..`, fail here rather than being lexically massaged into
    // the parent that happens to exist.
    char joined[SYS_OPEN_PATH_MAX + 1];
    int jl = territory_join_cwd(p->territory, path_scratch, path_len_raw,
                                joined, sizeof(joined));
    if (jl < 0)                                      return -1;

    // (2) Resolve the joined absolute path from the Territory root to verify it
    // exists, is a directory, and the caller holds X (search). stalk borrows
    // root (never refs/clunks it); RW-4 SA-F1: territory_root_ref takes the ref
    // ATOMICALLY under ns_lock (a plain read-then-ref raced a concurrent
    // pivot_root's swap+clunk-to-zero). Released at the uniform exit clunk below.
    struct Spoor *root = territory_root_ref(p->territory);
    if (!root)                                       return -1;
    struct Spoor *q = stalk(p, root, joined, (u64)jl, STALK_WALK, 0);
    spoor_clunk(root);
    if (!q)                                          return -1;

    // (3) CANONICALIZE for storage -- dot_path is getcwd's answer and the seed
    // for the next join, so it must stay clean (else `cd ..` would grow the
    // string without bound). Run on `joined`, which is already absolute, so
    // dot == NULL and dot_path is NOT re-read: a peer thread's concurrent
    // chdir cannot make the stored string disagree with the path stalk just
    // validated. Every component this pops was physically walked in (2), and
    // with no symlinks (G11) the lexical pop and stalk's trail pop consume the
    // same component sequence -- so `cleaned` names exactly what stalk landed
    // on. Computed before the perm gate below so a failure costs nothing.
    //
    // The output REUSES path_scratch, whose last read was the join in (1) --
    // so this step adds no stack (the handler already carries two
    // SYS_OPEN_PATH_MAX buffers, and a third would be ~19% of the 16 KiB
    // kernel stack in one frame, above a stalk() that nests its own trail).
    // The two buffers do not alias, and the cleaned form is never longer than
    // its input.
    char *cleaned = path_scratch;
    int cl = cwd_lexical_resolve((const char *)0, joined, (u64)jl,
                                 cleaned, sizeof(path_scratch));
    if (cl < 0)                                      { spoor_clunk(q); return -1; }

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

    // POSIX getcwd(buf, size) accepts ANY buffer large enough for the cwd -- do
    // NOT reject an oversized one. The pre-fix `buf_len_raw > SYS_OPEN_PATH_MAX+1
    // -> -1` broke every caller passing a PATH_MAX (4096) buffer -- GNU make,
    // clang, git, configure scripts, the near-universal `getcwd(buf, PATH_MAX)`
    // idiom (surfaced by the CL-1c make oracle; `make: getcwd: I/O error`). The
    // cwd is bounded by SYS_OPEN_PATH_MAX, so compute it FIRST, then validate +
    // copy EXACTLY len+1 bytes -- never the whole caller buffer. That both keeps
    // a huge buf_len_raw from overflowing the range check and matches POSIX
    // ("getcwd writes at most the pathname + NUL into the buffer").
    char scratch[SYS_OPEN_PATH_MAX + 1];
    int len = territory_getdot(p->territory, scratch, sizeof(scratch));
    if (len < 0)                                      return -1;
    if ((u64)len + 1 > buf_len_raw)                   return -1;   // path + NUL must fit the caller's buffer
    if (!sys_validate_user_buf(buf_va, (u64)len + 1)) return -1;

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

// =============================================================================
// PTY-1a (PTY-DESIGN.md section 4): POSIX sessions + process groups. Thin
// fronts over the proc.c cores (which hold g_proc_table_lock across find +
// validate + mutate). NON-static so the kernel tests call them directly (the
// LS-K identity-handler precedent). pid args arrive as u64 registers; the
// (int)(s64) cast preserves a negative pid, which the cores answer -T_E_SRCH
// (self-or-child lookup misses) rather than mis-treating as huge-positive.
// =============================================================================

s64 sys_setsid_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return (s64)proc_setsid(p);
}

s64 sys_setpgid_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return (s64)proc_setpgid(p, (int)(s64)a0, (int)(s64)a1);
}

s64 sys_getpgid_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return (s64)proc_getpgid(p, (int)(s64)a0);
}

s64 sys_getsid_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return (s64)proc_getsid(p, (int)(s64)a0);
}

// =============================================================================
// PTY-1c (PTY-DESIGN.md section 3): SYS_PTY_REGISTER -- the server-mediated
// (connection, qid) -> pts correlation. The registry cores (kernel/pts.c)
// gate on the minting server + the gen; this front resolves + gates the conn
// fd: it must be a SERVER-endpoint devsrv connection Spoor -- the SYS_SRV_
// ACCEPT product (aux = SrvConn, no CSRVCLIENT). The CLIENT-endpoint reject
// matters: a byte-mode client holds a CSRVCLIENT conn Spoor on the SAME
// SrvConn, and letting it register would let a CLIENT of a service claim
// (conn, qid) bindings the server never made. MINT additionally requires
// PROC_FLAG_MAY_POST_SERVICE (the service tier -- the Weft-7 F1 registry-
// squat lesson; only a flag holder can post a service and so be a server at
// all, making this defense-in-depth). NON-static for the kernel tests.
// =============================================================================

s64 sys_pty_register_for_proc(struct Proc *p, u64 op, u64 a1, u64 a2, u64 a3) {
    if (!p) return -T_E_INVAL;

    switch (op) {
    case PTY_REG_MINT:
    case PTY_REG_SLAVE: {
        if (op == PTY_REG_MINT && a3 != 0)        return -T_E_INVAL;
        if (op == PTY_REG_MINT && !proc_may_post_service(p))
            return -T_E_ACCES;
        // RIGHT_READ mirrors sys_srv_peer_for_proc: the accept installs
        // READ|WRITE on the endpoint; the register performs no I/O on it --
        // the endpoint + minting-server axes are the real authority.
        struct Spoor *sp = sys_lookup_spoor(p, (hidx_t)a1, RIGHT_READ);
        if (!sp)                                  return -T_E_INVAL;
        struct SrvConn *cn = devsrv_conn_of(sp);
        s64 r;
        if (!cn || (sp->flag & CSRVCLIENT)) {
            r = -T_E_INVAL;                       // not a server-endpoint conn
        } else if (op == PTY_REG_MINT) {
            r = pts_mint(p, cn, a2);
        } else {
            r = (s64)pts_bind_slave(p, cn, a2, a3);
        }
        // sp's ref held cn alive across the registry op (which took its own
        // binding ref); drop it last (#844 ref-transfer contract).
        spoor_clunk(sp);
        return r;
    }
    case PTY_REG_FREE:
        if (a2 != 0 || a3 != 0)                   return -T_E_INVAL;
        return (s64)pts_free(p, a1);
    default:
        return -T_E_INVAL;
    }
}

static s64 sys_pty_register_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return sys_pty_register_for_proc(p, a0, a1, a2, a3);
}

// =============================================================================
// PTY-1d: the tty seam + controlling-terminal fronts. SYS_TTY_SIGNAL takes a
// pts_id directly (the server holds it from MINT -- no fd). The fd-keyed
// three resolve the caller's own Spoor to its (SrvConn, qid) -- the pts.c
// cores gate on the registry entry (binding side, controlling session, gen).
// RIGHT_READ suffices on the fd: an O_RDONLY slave is still one's
// controlling terminal (POSIX), and no I/O runs here; the registry axes are
// the authority. The Spoor ref is held across the core (the extracted cn is
// only pointer-compared, but the hold keeps the chain trivially sound) and
// clunked on every path (#844). NON-static _for_proc bodies for the tests.
// =============================================================================

s64 sys_tty_signal_for_proc(struct Proc *p, u64 pts_id, u64 sig_class) {
    if (!p) return -T_E_INVAL;
    return pts_tty_signal(p, pts_id, (u32)sig_class);
}

s64 sys_tty_fd_op_for_proc(struct Proc *p, u64 fd_raw, u64 op_num, u64 arg) {
    if (!p) return -T_E_INVAL;
    struct Spoor *sp = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_READ);
    if (!sp) return -T_E_INVAL;
    struct SrvConn *cn = NULL;
    u64 qid = 0;
    s64 r = (s64)pts_spoor_conn_qid(sp, &cn, &qid);
    if (r == 0) {
        switch (op_num) {
        case SYS_TTY_ACQUIRE: r = pts_tty_acquire(p, cn, qid);            break;
        case SYS_TTY_SET_FG:  r = pts_tty_set_fg(p, cn, qid, (u32)arg);   break;
        case SYS_TTY_CONT:    r = pts_tty_cont(p, cn, qid, (u32)arg);     break;
        default:              r = pts_tty_get_fg(p, cn, qid);             break;
        }
    }
    spoor_clunk(sp);
    return r;
}

static s64 sys_tty_signal_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return sys_tty_signal_for_proc(p, a0, a1);
}

static s64 sys_tty_acquire_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return sys_tty_fd_op_for_proc(p, a0, SYS_TTY_ACQUIRE, 0);
}

static s64 sys_tty_set_fg_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return sys_tty_fd_op_for_proc(p, a0, SYS_TTY_SET_FG, a1);
}

static s64 sys_tty_get_fg_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a1; (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return sys_tty_fd_op_for_proc(p, a0, SYS_TTY_GET_FG, 0);
}

static s64 sys_tty_cont_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a2; (void)a3;
    struct Thread *t = current_thread();             if (!t) return -1;
    struct Proc *p = t->proc;                        if (!p) return -1;
    return sys_tty_fd_op_for_proc(p, a0, SYS_TTY_CONT, a1);
}

// SYS_YIELD (#33): the thin syscall front for sched_yield_hint (sched.c) --
// the whole contract lives there. Always 0 (POSIX sched_yield(2)); whether a
// dispatch happened is deliberately not surfaced (a hint has no observable
// success/failure, and callers loop regardless). Static: the kernel tests
// exercise sched_yield_hint directly; the in-guest consumers prove dispatch.
static s64 sys_yield_handler(u64 a0, u64 a1, u64 a2, u64 a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    (void)sched_yield_hint();
    return 0;
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

// VIVARIUM V-5: the resolution + install core, taking the path in KERNEL
// memory. sys_open_handler is this with a user-buffer copy in front; the
// phenotype's socket translators call it directly with paths they built
// themselves ("/net/tcp/clone", "/net/tcp/7/data").
//
// EXTRACTED RATHER THAN DUPLICATED, which the I-43 row requires of every T2
// shell: there is exactly ONE implementation of "resolve a path in this Proc's
// Territory and install the result as an fd", so a socket open passes through
// the same stalk, the same per-component perm_check, and the same
// omode-derived rights as any other open. A second copy would be a second
// place for a gate to go missing.
static s64 sys_open_kpath_for_proc(struct Proc *p, u64 start_fd_raw,
                                   const char *kpath, u64 klen, u64 omode_raw) {
    if (!p || !kpath)                                return -1;
    if (klen == 0)                                   return -1;
    if (klen > SYS_OPEN_PATH_MAX)                    return -1;
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

    const char *path_scratch = kpath;

    // LS-4: a RELATIVE path with the FROM_ROOT sentinel resolves against the
    // Territory cwd (dot) -- POSIX openat(AT_FDCWD, ...). Join dot + path into
    // an absolute path, then resolve from root (start is already root_spoor).
    // An explicit start-fd (a dirfd) or an absolute path is unchanged. stalk
    // still re-clamps ".." at root_spoor, so the join cannot escape containment
    // (I-28 preserved; no new mechanism).
    //
    // #83: the join is VERBATIM -- "."/".."/a trailing separator survive into
    // `joined` so stalk gates them exactly as it gates the absolute spelling.
    // Collapsing them here popped never-walked components, so `f/..`, `f/.`,
    // `f/` and even `nonexistent/..` opened successfully.
    char joined[SYS_OPEN_PATH_MAX + 1];
    const char *rpath = path_scratch;
    u64 rlen = klen;
    if (start_fd_raw == SYS_WALK_OPEN_FROM_ROOT && path_scratch[0] != '/') {
        int jl = territory_join_cwd(p->territory, path_scratch, klen,
                                    joined, sizeof(joined));
        if (jl < 0) { spoor_clunk(start); return -1; }
        rpath = joined;
        rlen  = (u64)jl;
    }

    int amode = (omode_raw & SYS_WALK_OPEN_OPATH) ? STALK_WALK : STALK_OPEN;
    // D-1: thread the no-follow-final flag into the resolver, and STRIP it
    // from the omode passed onward -- Dev.open / the Tlopen mode must never
    // see it (a resolution directive, not an open mode). Composes with OPATH:
    // STALK_WALK|STALK_NOFOLLOW returns the LINK itself as the navigation
    // handle (the Linux O_PATH|O_NOFOLLOW lstat-fd idiom -- SYS_FSTAT on it
    // reports the link's own metadata).
    if (omode_raw & SYS_WALK_OPEN_NOFOLLOW) amode |= STALK_NOFOLLOW;
    u32 omode_eff = (u32)(omode_raw & ~(u64)SYS_WALK_OPEN_NOFOLLOW);
    // errno-rollout (ER-1): stalk writes the cause (T_E_NOENT walk-miss,
    // T_E_ACCES perm denial, ...) so SYS_OPEN returns the real -errno. This is
    // the Go-build keystone: a missing path -> -T_E_NOENT (Go os.IsNotExist
    // true -> the O_CREATE create-or-open fallback fires; the cache existence
    // checks work) instead of the bare -1 (Go renders that EPERM).
    int serr = T_E_NOENT;
    struct Spoor *quarry = stalk_err(p, start, rpath, rlen,
                                     amode, omode_eff, &serr);
    // #844: start (BORROWED by stalk -- it never refs/clunks it) is done now;
    // release the borrow. quarry owns its own ref from stalk.
    spoor_clunk(start);
    if (!quarry)                                     return -serr;

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

// SYS_OPEN: the user-buffer front half of sys_open_kpath_for_proc. Everything
// below the copy -- base resolution, the cwd join, stalk, rights, install --
// lives in the core so the phenotype's kernel-path callers share it exactly.
static s64 sys_open_handler(u64 start_fd_raw, u64 path_va,
                            u64 path_len_raw, u64 omode_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    if (path_len_raw == 0)                           return -1;
    if (path_len_raw > SYS_OPEN_PATH_MAX)            return -1;
    if (!sys_validate_user_buf(path_va, path_len_raw)) return -1;

    // Copy the path into kernel scratch + reject embedded NUL (truncation /
    // wire-leak vector). '/' is ALLOWED here (multi-component) — stalk
    // tokenizes it. The scratch is one byte over so the NUL terminator below
    // is always writable even at the max length.
    char path_scratch[SYS_OPEN_PATH_MAX + 1];
    for (u64 i = 0; i < path_len_raw; i++) {
        u8 b;
        if (uaccess_load_u8(path_va + i, &b) != 0)   return -1;
        if (b == '\0')                               return -1;
        path_scratch[i] = (char)b;
    }
    path_scratch[path_len_raw] = '\0';

    return sys_open_kpath_for_proc(p, start_fd_raw, path_scratch,
                                   path_len_raw, omode_raw);
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
    if (name_len_raw == 0)                            return -T_E_INVAL;
    if (name_len_raw > SYS_WALK_OPEN_NAME_MAX)        return -T_E_INVAL;
    if (!sys_validate_user_buf(name_va, name_len_raw)) return -T_E_FAULT;

    // omode bit validation (reject unknown bits; forward-compat).
    if (omode_raw & ~(u64)SYS_WALK_OPEN_OMODE_VALID)  return -T_E_INVAL;

    // perm bit validation: only the low-9 mode bits + DMDIR are permitted at
    // v1.0. Any other DM* bit (DMAPPEND / DMEXCL / DMTMP / ...) -> -1, so a
    // future bit cannot be silently dropped. Also reject the full 64-bit raw
    // having bits above 32 (perm is a u32 ABI field).
    if (perm_raw & ~(u64)SYS_WALK_CREATE_PERM_VALID)  return -T_E_INVAL;
    u32 perm = (u32)perm_raw;

    // Resolve the parent directory Spoor. RIGHT_WRITE is the gate: create
    // mutates the directory's contents. (SYS_WALK_OPEN uses RIGHT_READ; create
    // is the write-side op.) The FROM_ROOT sentinel walks from the caller's
    // pivoted Territory root, same as SYS_WALK_OPEN.
    struct Spoor *src;
    if (parent_fd_raw == SYS_WALK_OPEN_FROM_ROOT) {
        if (!p->territory)                            return -T_E_INVAL;
        // RW-4 SA-F1: atomic read+ref under ns_lock (closes the read-then-ref
        // UAF window vs a concurrent pivot_root).
        src = territory_root_ref(p->territory);
        if (!src)                                     return -T_E_INVAL;
    } else {
        src = sys_lookup_spoor(p, (hidx_t)parent_fd_raw, RIGHT_WRITE);   // ref-held
        if (!src)                                     return -T_E_BADF;
    }
    // #80: split the flat reject, as on the open side. The .walk arm is
    // DEFENSIVE (all 18 Devs fill the slot); the .create arm is LIVE -- a Dev
    // with no create slot genuinely cannot create, and OPNOTSUPP says so.
    if (!src->dev || !src->dev->walk)   { spoor_clunk(src); return -T_E_NOTDIR; }
    if (!src->dev->create)              { spoor_clunk(src); return -T_E_OPNOTSUPP; }
    // #81: creating INTO a non-directory -- the gate #80 recorded as owed here.
    // #80 framed it as disambiguating the clone-walk NULL below; MEASURING it
    // showed that is not the reachable case. Creating into a 0644 file answered
    // -T_E_ACCES, because the W|X check below denied on the file's mode long
    // before any clone-walk was attempted. So the gate's real job is the same
    // as #79's: make the answer mode-INDEPENDENT by testing type first.
    // (Disambiguating the clone-walk NULL remains true and is now moot for the
    // common case, since a non-directory no longer reaches it.)
    if (!(src->qid.type & QTDIR))       { spoor_clunk(src); return -T_E_NOTDIR; }

    // A-2d: write + search (W|X) permission on the parent directory before
    // creating in it. Gated on perm_enforced -- LIVE for dev9p since the A-3b
    // flip (Stratum-backed trees enforce rwx); devramfs is read-only (its
    // .create stub returns NULL) so this is effectively dead there, but correct:
    // a non-system principal lacks other-w on a 0755 dir and is denied here
    // before the create attempt. fail-closed on no stat.
    if (src->dev->perm_enforced) {
        struct t_stat parent_st;
        if (spoor_stat_native(src, &parent_st) != 0)          { spoor_clunk(src); return -T_E_IO; }
        if (perm_check(p, &parent_st, PERM_W | PERM_X) != 0)  { spoor_clunk(src); return -T_E_ACCES; }
    }

    // Copy + validate the component name (same strict shape as SYS_WALK_OPEN:
    // reject '/' '\0' "." ".."; NUL-terminate for dev9p's strlen scan).
    char name_scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    for (u64 i = 0; i < name_len_raw; i++) {
        u8 b;
        // #80: bad caller page -> EFAULT; forbidden component byte -> EINVAL.
        if (uaccess_load_u8(name_va + i, &b) != 0)    { spoor_clunk(src); return -T_E_FAULT; }
        if (b == '/' || b == '\0')                    { spoor_clunk(src); return -T_E_INVAL; }
        name_scratch[i] = (char)b;
    }
    if (name_len_raw == 1 && name_scratch[0] == '.')  { spoor_clunk(src); return -T_E_INVAL; }
    if (name_len_raw == 2 && name_scratch[0] == '.' &&
                              name_scratch[1] == '.') { spoor_clunk(src); return -T_E_INVAL; }
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
        if (perm & ~(SYS_WALK_CREATE_DMSRVBYTE |
                     SYS_WALK_CREATE_DMSRVBULK))        { spoor_clunk(src); return -T_E_INVAL; }
        enum srv_mode mode = (perm & SYS_WALK_CREATE_DMSRVBYTE)
                                 ? SRV_MODE_BYTE : SRV_MODE_9P;
        bool bulk = (perm & SYS_WALK_CREATE_DMSRVBULK) != 0;   // CF-3 B ring class
        // #844: devsrv_post_listener mints a registry-lifetime KObj_Srv (not
        // tied to src); release the src borrow after it returns.
        s64 lh = (s64)devsrv_post_listener(p, src, name_scratch,
                                           (size_t)name_len_raw, mode, bulk);
        spoor_clunk(src);
        return lh;
    }

    // DMSRVBYTE / DMSRVBULK are meaningful ONLY for the /srv service post
    // above. On a regular create they must not reach a Dev's create perm
    // (e.g. a dev9p Tlcreate), where the high bits would corrupt the wire
    // perm -- reject them.
    if (perm & (SYS_WALK_CREATE_DMSRVBYTE |
                SYS_WALK_CREATE_DMSRVBULK))               { spoor_clunk(src); return -T_E_INVAL; }

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
    if (!nc)                                          { spoor_clunk(src); return -T_E_NOMEM; }

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
        // #80: this arm is genuinely ambiguous -- a leaf Dev (cons/null/zero,
        // per shape (a) above) answers NULL because it is not a directory,
        // while dev9p can answer NULL on fid-pool exhaustion. EIO is the honest
        // choice; reporting ENOTDIR would be a lie in the second case. Making
        // the not-a-directory half precise wants a qid.type gate on the parent
        // -- the single-hop twin of the #79 stalk gate -- which is a resolution
        // change, not an errno one, and is tracked with #81 rather than
        // smuggled into this rollout.
        return -T_E_IO;
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
        return -T_E_IO;      // #80: Dev broke the reuse-nc contract
    }
    walkqid_free(w);

    // Create + open the new object. dev->create returns nc (opened) on
    // success or NULL on failure; on NULL nc still owns its walked fid, so
    // spoor_clunk runs dev->close -> clunks it.
    // D-1: strip the resolution flag (vacuous for create -- a created child is
    // never a symlink -- but it must not reach a Dev or be stored in Spoor.mode).
    u32 omode_dev = (u32)(omode_raw & ~(u64)SYS_WALK_OPEN_NOFOLLOW);
    struct Spoor *opened = nc->dev->create(nc, name_scratch, (int)omode_dev,
                                            perm, p->primary_gid);
    if (!opened) {
        // #99 (#102 errno-loss): propagate the real create errno the Dev
        // recorded (dev9p maps its Tlcreate/Tmkdir Rlerror -- e.g. -EEXIST on a
        // racing/duplicate create -- into a passthrough-range errno). Without
        // this the bare -1 reaches EL0 as a blanket EPERM (go's native seam) or
        // EIO (pouch), so os.OpenFile(O_CREATE) could not distinguish
        // "already exists" from a real failure and fall back to opening it.
        // Self-gating: dev9p_create_errno returns -1 for a non-dev9p Spoor or an
        // unrecorded/out-of-range value, preserving the prior behavior there.
        s64 cerr = dev9p_create_errno(nc);
        spoor_clunk(nc);
        return cerr;
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
        return -T_E_IO;      // #80: Dev broke the reuse-nc contract
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
        return -T_E_NOMEM;   // #80: full fd table (EMFILE unregistered)
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
    // Area-F errno rollout: propagate the Dev's real -errno (dev9p returns
    // -(T_E_*)); a legacy Dev's bare -1 stays the generic sentinel.
    return (s64)rc;
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

    if (buf_len_raw == 0 || buf_len_raw > SYS_RW_STACK) return -1;
    if (!sys_validate_user_buf(buf_va, buf_len_raw))  return -1;

    // #844: c is REF-HELD (borrow); spoor_clunk on every exit (readdir blocks).
    struct Spoor *c = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_READ);
    if (!c)                                          return -1;
    // #81: a T_OPATH navigation handle is NOT a byte-I/O channel -- reject readdir
    // too (listing a dir's entries is content the perm_check-exempt O_PATH open
    // would otherwise leak for a non-readable dir). IDENTITY-DESIGN 9.4 #81.
    if (c->flag & CWALKONLY)                       { spoor_clunk(c); return -1; }
    if (!c->dev || !c->dev->readdir)               { spoor_clunk(c); return -1; }

    u8 scratch[SYS_RW_STACK];
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
    if (sys_copy_component(oldname_va, oldname_len_raw, old_scratch) != 0) return -T_E_INVAL;
    if (sys_copy_component(newname_va, newname_len_raw, new_scratch) != 0) return -T_E_INVAL;

    // #844: od + nd are REF-HELD borrows; spoor_clunk BOTH on every exit. od==nd
    // (same dir fd / both FROM_ROOT) means each resolve took a ref -> two clunks
    // balance. Held across the (possibly blocking) stat + rename 9P ops.
    struct Spoor *od = sys_resolve_dir_wr(p, olddir_fd_raw);
    if (!od)                                          return -T_E_BADF;
    struct Spoor *nd = sys_resolve_dir_wr(p, newdir_fd_raw);
    if (!nd)                                        { spoor_clunk(od); return -T_E_BADF; }
    // #80: a Dev with no .rename slot cannot perform the operation at all --
    // OPNOTSUPP, distinguishable from every verdict the slot itself can return
    // (devramfs leaves it NULL, so `mv` inside the boot ramfs says so plainly
    // instead of reporting a generic I/O error).
    if (!od->dev || !od->dev->rename)              { spoor_clunk(od); spoor_clunk(nd); return -T_E_OPNOTSUPP; }
    // Two-cursor + cross-Dev invariant: a 9P renameat is within ONE server, so
    // both directories MUST be on the same Dev (dev9p_rename adds the same-
    // session guard). Rejected here before any Dev op.
    //
    // #80 seam: POSIX names this case EXDEV (18), which a caller like `mv` reads
    // as "fall back to copy+unlink". EXDEV is not in the errno registry, and
    // adding it is an ERRORS.md append needing signoff -- so this stays EINVAL
    // for now and the cross-Dev copy fallback remains the caller's own policy.
    if (od->dev != nd->dev)                        { spoor_clunk(od); spoor_clunk(nd); return -T_E_INVAL; }

    // A-3b (closes A-2d audit F2): rwx enforcement on dir mutation. POSIX
    // rename needs write + search (W|X) on BOTH parent dirs. Gated on the
    // Dev's perm_enforced (devramfs leaves .rename NULL; dev9p enforces from
    // A-3b). od->dev == nd->dev here, so one flag governs both.
    if (od->dev->perm_enforced) {
        struct t_stat ost, nst;
        if (spoor_stat_native(od, &ost) != 0)             { spoor_clunk(od); spoor_clunk(nd); return -T_E_IO; }
        if (perm_check(p, &ost, PERM_W | PERM_X) != 0)    { spoor_clunk(od); spoor_clunk(nd); return -T_E_ACCES; }
        if (spoor_stat_native(nd, &nst) != 0)             { spoor_clunk(od); spoor_clunk(nd); return -T_E_IO; }
        if (perm_check(p, &nst, PERM_W | PERM_X) != 0)    { spoor_clunk(od); spoor_clunk(nd); return -T_E_ACCES; }
    }

    // #80: the Dev now returns a specific -T_E_* (see the .rename contract in
    // <thylacine/dev.h>); forward it verbatim rather than flattening to -1.
    int rc = od->dev->rename(od, old_scratch, nd, new_scratch);
    spoor_clunk(od);
    spoor_clunk(nd);
    return rc;
}

static s64 sys_unlink_handler(u64 parent_fd_raw, u64 name_va, u64 name_len_raw,
                               u64 flags_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;

    // Only 0 or SYS_UNLINK_REMOVEDIR permitted; any other bit -> EINVAL (so a
    // future flag cannot be silently dropped, same discipline as
    // SYS_WALK_CREATE perm).
    if (flags_raw & ~(u64)SYS_UNLINK_REMOVEDIR)       return -T_E_INVAL;

    char scratch[SYS_WALK_OPEN_NAME_MAX + 1];
    if (sys_copy_component(name_va, name_len_raw, scratch) != 0) return -T_E_INVAL;

    // #844: c is a REF-HELD borrow; spoor_clunk on every exit (held across the
    // possibly-blocking stat + unlink 9P ops).
    struct Spoor *c = sys_resolve_dir_wr(p, parent_fd_raw);
    if (!c)                                          return -T_E_BADF;
    // #80: no .unlink slot => this Dev cannot remove at all (devramfs) --
    // OPNOTSUPP, distinct from any verdict the slot itself returns.
    if (!c->dev || !c->dev->unlink)                { spoor_clunk(c); return -T_E_OPNOTSUPP; }

    // A-3b (closes A-2d audit F2): W|X on the parent dir to remove an entry
    // (POSIX). Gated on perm_enforced (dev9p enforces from A-3b; devramfs
    // leaves .unlink NULL).
    if (c->dev->perm_enforced) {
        struct t_stat cst;
        if (spoor_stat_native(c, &cst) != 0)              { spoor_clunk(c); return -T_E_IO; }
        if (perm_check(p, &cst, PERM_W | PERM_X) != 0)    { spoor_clunk(c); return -T_E_ACCES; }
    }

    // #80: the Dev now returns a specific -T_E_* (see the .unlink contract in
    // <thylacine/dev.h>); forward it verbatim. This is what lets a caller
    // distinguish "that is a directory" from "that directory is not empty" from
    // "you may not write here" -- all three were one flat -1 before.
    int rc = c->dev->unlink(c, scratch, (u32)flags_raw);
    spoor_clunk(c);
    return rc;
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
                              u32 uid, u32 gid, u64 size) {
    if (!c)                                          return -1;
    if (!c->dev || !c->dev->wstat_native)            return -1;
    return c->dev->wstat_native(c, valid, mode, uid, gid, size);
}

// Inner — testable without a live EL0 thread (all-scalar args, no user
// buffer). The handler thins to current_thread() + this.
s64 sys_wstat_for_proc(struct Proc *p, hidx_t h, u32 valid, u32 mode,
                       u32 uid, u32 gid, u64 size) {
    if (!p)                                          return -1;

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
    if (valid & T_WSTAT_SIZE) {
        // The s64 offset domain (the SYS_LSEEK/SYS_PREAD bound): a length
        // whose sign bit is set cannot be represented by the size_t/off_t
        // plumbing below and is rejected up front.
        if (size > 0x7FFFFFFFFFFFFFFFull)            return -1;
    } else {
        size = 0;
    }

    // #47 (the #46 sibling): kind-gate only -- KOBJ_SPOOR, ANY rights (rights
    // mask 0, the SYS_FSTAT/#46 + SYS_LSEEK posture); rejects KOBJ_SRV. POSIX
    // fchmod(2)/fchown(2) work on an fd opened O_RDONLY: the authority to
    // change metadata is the IDENTITY axis (owner-or-CAP, perm_wstat_check
    // below), never the handle's byte-I/O envelope. An O_RDONLY open mints a
    // RIGHT_READ-only handle (A-3 F1 omode-derived rights), so the old
    // RIGHT_WRITE gate made fchmod on it fail -1 while guarding nothing --
    // the caller can re-walk the path and wstat that handle; the fd is just a
    // name for the file. The endowed-fd exception documented at #46 (a
    // rights-stripped handle passed cross-Proc still wstats IF the receiver
    // passes perm_wstat_check) carries over: rights bound byte I/O + transfer,
    // never the identity axis, and POSIX fd-passing behaves identically.
    // #844: c is REF-HELD; spoor_clunk on every exit -- the ref keeps c alive
    // across the (possibly blocking) stat/setattr.
    // T_WSTAT_SIZE is a CONTENT mutation (POSIX ftruncate(2)): unlike the
    // #47 kind-gate-only metadata axes it requires the fd's byte-I/O WRITE
    // envelope -- an O_RDONLY fd must not truncate. Under the A-3 F1
    // omode-derived rights RIGHT_WRITE == "opened for writing", and the
    // A-2d open-time perm_check already enforced the identity W axis for
    // that open, so no perm_wstat_check applies to the size axis below.
    u32 want_rights = (valid & T_WSTAT_SIZE) ? RIGHT_WRITE : 0;
    struct Spoor *c = sys_lookup_rw_handle(p, h, want_rights);
    if (!c)                                          return -1;

    // #81 class, extended to the size axis: an O_PATH (CWALKONLY) handle is
    // born RIGHT_WRITE but is perm_check-EXEMPT at open (it needs only path
    // X-search, not W) -- so its RIGHT_WRITE is hollow. The metadata axes are
    // safe (perm_wstat_check below is their real authority, applied regardless
    // of the handle rights), but T_WSTAT_SIZE deliberately relies on
    // RIGHT_WRITE as its sole gate, so a truncate through an O_PATH fd would
    // mutate a file the caller has no W permission on. Reject it -- a
    // navigation handle is not a byte-I/O channel, and truncate IS byte I/O.
    if ((valid & T_WSTAT_SIZE) && (c->flag & CWALKONLY)) {
        spoor_clunk(c);
        return -1;
    }

    // A-2d: the ownership-change policy (IDENTITY-DESIGN.md 3.7.1 + perm.c).
    // Gated on perm_enforced (dev9p live since A-3; devramfs .wstat_native is
    // NULL so SYS_WSTAT on it returns -1 below regardless). Reads the file's
    // CURRENT owner, then applies the policy -- for the METADATA axes only:
    // since #47 this identity check is the ONLY write-authority gate on the
    // mode/uid/gid path -- do not weaken it. A size-only call skips it (its
    // identity check was the open-time perm_check, above).
    if ((valid & (T_WSTAT_MODE | T_WSTAT_UID | T_WSTAT_GID)) &&
        c->dev && c->dev->perm_enforced) {
        struct t_stat cur;
        if (spoor_stat_native(c, &cur) != 0)              { spoor_clunk(c); return -1; }
        if (perm_wstat_check(p, cur.uid, valid, gid) != 0){ spoor_clunk(c); return -1; }
    }
    int rc = spoor_wstat_native(c, valid, mode, uid, gid, size);
    spoor_clunk(c);
    return rc == 0 ? 0 : -1;
}

static s64 sys_wstat_handler(u64 hraw, u64 valid_raw, u64 mode_raw,
                             u64 uid_raw, u64 gid_raw, u64 size_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    struct Proc *p = t->proc;
    if (!p)                                          return -1;
    return sys_wstat_for_proc(p, (hidx_t)hraw, (u32)valid_raw, (u32)mode_raw,
                              (u32)uid_raw, (u32)gid_raw, size_raw);
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

    // The root must be a DIRECTORY -- the #81 single-hop gate, applied to the
    // one other place a Spoor becomes a resolution base. Installing a non-dir
    // wedges the Territory: every later resolution answers T_E_NOTDIR at its
    // first component, exec-from-namespace fails, and territory_root_ref hands
    // the same node to D-1's absolute-target re-anchor. Contained (the Proc only
    // wedges itself, I-1) but it is an unprivileged self-wedge with no use.
    // Pre-existing -- t_chroot of an O_PATH handle on a FILE did this before
    // D-1 too -- but D-1 shipped File::open_link, a documented API whose whole
    // job is to hand back a non-directory, so the shape is now easy to reach.
    if (!(source->qid.type & QTDIR))                 { spoor_clunk(source); return -1; }

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
    if (!p->as)                                      return -T_E_INVAL;

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
// #15: no longer noreturn -- the STOP and IGNORE dispositions return.
extern int  notes_noted_default(struct exception_context *ctx,
                                struct Thread *t);

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
        // NDFLT -- the note's TRUE default action, per-note since #15
        // (notes_default_action + the g_known_notes `dfl` column). Three
        // outcomes: TERMINATE never returns; STOP and IGNORE restore the
        // pre-handler context (rewriting ctx, so regs[0] below is NOT what
        // EL0 sees) and return 0. A STOP additionally arms job_stop_req; the
        // thread parks at the EL0-return tail after the die-check, so a
        // racing group-terminate still wins.
        //
        // The terminating arm's history, which the per-note table did not
        // change: post-#811 (ARCH 8.8.1) exits() on a Proc with live peers
        // CASCADES via proc_group_terminate rather than extincting, so a
        // multi-thread Proc's uncaught default-terminate takes down the whole
        // Proc -- the POSIX default action. RW-8 R5-F1 removed the prior
        // live-peers refusal (-1), which predated #809/#811 and, with pouch's
        // always-installed handler bypassing the LS-5 kernel default-
        // terminate, silently swallowed SIGINT/SIGTERM in multi-thread pouch
        // daemons (NDFLT refused -> bootstrap NCONT-resumes -> the signal
        // evaporates).
        if (notes_noted_default(ctx, t) != 0) {
            ctx->regs[0] = (u64)(s64)-1;
        }
        return;
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

// N-4 in ONE place: `kill` terminates its target, whatever the target's thread
// count and whatever state its note ring is in. Returns true iff `name` was
// `kill` and the cascade ran, so a caller's remaining work is the not-a-kill
// path only.
//
// It exists because the two SYS_POSTNOTE arms (self and cross) each carried
// their own spelling of this rule and DRIFTED: aux#241 removed the cross arm's
// `live_threads > 1` gate and left the self arm's, and round-2 F4 then found
// the self arm failing a kill on a full ring. Two arms agreeing by inspection
// is what produced both defects; sharing the decision is the fix that outlives
// either one. A third spelling already exists in devproc.c's `/proc/<pid>/ctl`
// kill verb -- fold it in here if that file is ever touched for this reason.
//
// CALLER MUST HOLD g_proc_table_lock: proc_group_terminate's universal
// death-wake walks p->threads (#811, ARCH section 8.8.1). The cross arm holds
// it via proc_for_each; the self arm takes it around this call.
static bool postnote_kill_cascade_locked(struct Proc *target, const char *name) {
    if (!notes_name_is_kill(name)) return false;
    proc_group_terminate(target, "killed");
    return true;
}

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

    // `kill` cascade-terminates the whole Proc, whatever its thread count
    // (ARCH §7.9.1, I-24). proc_group_terminate flags it + wakes/kicks its
    // Threads so each self-exits at its EL0-return die-check; the last Thread
    // out reaps the Proc. Safe under g_proc_table_lock (held by proc_for_each
    // here): proc_group_terminate acquires only torpor / rendez / cs locks,
    // all below proc_table_lock in the order, and the target is alive under
    // this lock so there is no reap-UAF.
    //
    // #241: this used to be gated on `live_threads > 1`, letting a SINGLE-
    // thread target fall through to the note post below "-- the existing
    // non-catchable-kill EL0-return delivery, left unchanged". That sentence
    // was true about the delivery and blind to the fact that a thread can be
    // somewhere OTHER than on its way to EL0. `kill` arms no terminate latch
    // (notes_name_terminate_latch returns 0 for NOTE_BIT_KILL), so the queued
    // note woke NOTHING -- and a job-stopped target parked in
    // el0_return_stop_check has exactly three exits (group_exit_msg,
    // !proc_stop_requested, thread_die_pending), none of which a latchless
    // queued kill satisfies. SYS_POSTNOTE returned SUCCESS and the Proc lived
    // forever: the job stop had caught the uncatchable note, violating N-4.
    //
    // Routing through proc_group_terminate closes it at the root rather than
    // per-park: group_exit_msg is the ONE signal every park and sleep
    // predicate already honours, so this also covers a target merely blocked
    // (where the latchless kill previously waited for some unrelated wake).
    // Not a semantics change -- thread_exit_self's become_zombie arm derives
    // its status from group_exit_msg with the same `"ok" -> 0 / else -> 1`
    // collapse exits() uses, so the observable outcome is exit_msg "killed" /
    // status 1 either way. It also makes SYS_POSTNOTE agree with the /proc/
    // <pid>/ctl `kill` verb, which has always dispatched via
    // proc_group_terminate uniformly (devproc.c).
    //
    // Round-2 F4 (aux#253): the SELF arm used to keep its thread-count gate,
    // and the paragraph here used to argue that was deliberate. It named a real
    // property (a self-kill cannot be SWALLOWED by a stop, since the tail
    // delivers notes before el0_return_stop_check) and mistook it for the whole
    // obligation -- a full note ring made the self-kill fail for want of space.
    // Both arms now route through the ONE predicate below.
    if (postnote_kill_cascade_locked(target, w->name)) {
        w->result = 1;
        return 1;
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

// Test support (the mmap_eager_copy_for_test convention). Deliberately absent
// from any header -- the harness extern-declares it, and there is no
// production caller. It runs the REAL cross-Proc post: proc_for_each +
// postnote_walk_cb under g_proc_table_lock, exactly as sys_postnote_handler
// does, rather than a reimplementation the test could quietly drift from.
// Returns the walk's canonical result (+1 posted / -1 refused / 0 not found).
int sys_postnote_cross_for_test(struct Proc *caller, int target_pid,
                                const char *name);
int sys_postnote_cross_for_test(struct Proc *caller, int target_pid,
                                const char *name) {
    struct postnote_walk_ctx wctx = {
        .target_pid = target_pid,
        .caller     = caller,
        .name       = name,
        .result     = 0,
    };
    (void)proc_for_each(postnote_walk_cb, &wctx);
    return wctx.result;
}

// The SELF arm, extracted from sys_postnote_handler so it has a name a test can
// call (aux#253). The handler resolves `p` from current_thread(); everything
// after that resolution uses only `p` and the validated name, so the arm splits
// cleanly here and the syscall keeps driving THIS function rather than a copy.
//
// Returns the syscall's own value: 0 posted-or-killed, -1 refused.
static s64 postnote_self(struct Proc *p, const char *name) {
    // Unlike the cross arm we are not already under g_proc_table_lock, so take
    // it around the cascade. `p` is self and cannot be reaped while running it.
    irq_state_t s = proc_table_lock_acquire();
    bool killed = postnote_kill_cascade_locked(p, name);
    proc_table_lock_release(s);
    if (killed) return 0;

    int rc = notes_post(p, name, 0u, p, false);
    // LS-5c (P3-terminate): a self-post of `interrupt` in a multi-thread Proc
    // may arm the terminate latch while a PEER thread is blocked in a rendez
    // sleep -- wake the peers so they unwind to their tails (the posting thread
    // itself is running and reaches its own tail at this syscall's return). The
    // wake walks p->threads, so it needs g_proc_table_lock (the #811 contract);
    // take it only when the latch armed (the read is a benign pre-check -- the
    // wake re-validates under its own internal gate, and `p` is self, immune to
    // reap here).
    if (rc == 0 && proc_intr_terminate_pending(p)) {
        irq_state_t ws = proc_table_lock_acquire();
        proc_interrupt_terminate_wake(p);
        proc_table_lock_release(ws);
    }
    return (rc == 0) ? 0 : (s64)-1;
}

// Test support, the sys_postnote_cross_for_test twin: drives the REAL self arm
// with an explicit caller, because the production entry takes its target from
// current_thread() and a kernel unit test's current thread is the harness's.
// Deliberately absent from any header; no production caller.
s64 sys_postnote_self_for_test(struct Proc *caller, const char *name);
s64 sys_postnote_self_for_test(struct Proc *caller, const char *name) {
    return postnote_self(caller, name);
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
    // self-kill cascades the whole Proc instead of being refused (the prior
    // `kill -> -EIO`, 13b R1-F9). The caller returns success + self-exits at
    // its own EL0-return die-check before userspace resumes. The arm's body and
    // its lock discipline live on postnote_self; WHY the kill decision is
    // shared with the cross arm (round-2 F4, aux#253) lives on
    // postnote_kill_cascade_locked.
    if (target_pid == p->pid || pid_raw == 0) return postnote_self(p, buf);

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

    spin_lock(&p->as->lock);

    // Pick a free VA — first-fit in the burrow-attach window. The gap is
    // chosen and the VMA installed under one lock hold, so a sibling
    // thread's concurrent attach cannot claim the same gap.
    u64 vaddr;
    if (vma_find_gap(p, length, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &vaddr) != 0) {
        spin_unlock(&p->as->lock);
        return -1;
    }

    // #65 (I-32): charge this Proc's anon-page floor BEFORE committing the
    // eager allocation. Under vma_lock, so the check+charge is atomic against a
    // sibling attach (the cap is exact). A non-TCB Proc over PROC_PAGE_MAX is
    // refused with -ENOMEM here -- it never reaches the allocator. Uncharged on
    // every failure path below + on SYS_BURROW_DETACH.
    //
    // #106: charge what the buddy actually TAKES, not what was asked for. The
    // comment on burrow_create_anon below has always said "power-of-2 rounded"
    // -- the charge just never acted on it, so a Proc attaching 2049-page
    // regions occupied 4096 pages each while being billed 2049. Bounded at 2x,
    // but a floor a Proc can silently stand twice as high as is not the floor
    // I-32 claims. burrow_backing_pages IS the allocator's own answer; the
    // uncharge at detach recomputes it from the same length.
    //
    // Fits u32: length is bounded by BURROW_ATTACH_MAX (256 MiB) above, whose
    // rounded page count is 65536.
    u32 npages = (u32)burrow_backing_pages(length);
    if (!proc_page_charge(p, npages)) {
        spin_unlock(&p->as->lock);
        return -T_E_NOMEM;
    }

    // burrow_create_anon: handle_count = 1 (the construction reference),
    // mapping_count = 0; pages allocated eagerly (power-of-2 rounded).
    struct Burrow *b = burrow_create_anon(length);
    if (!b) {
        proc_page_uncharge(p, npages);
        spin_unlock(&p->as->lock);
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
        spin_unlock(&p->as->lock);
        return -1;
    }
    // #131/#132: stamp the payer BEFORE dropping the construction handle, so
    // the record exists for every path that can later settle this region --
    // including one in another Proc, after this one has walked away. Recorded
    // only on success: the failure paths above uncharge directly and free the
    // Burrow, so there is nothing left to attribute.
    burrow_charge_record(b, p, npages);
    burrow_unref(b);

    spin_unlock(&p->as->lock);

    // vaddr is in [EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP) — far
    // below the s64 sign bit, so a valid base is never mistaken for -1.
    return (s64)vaddr;
}

static s64 sys_burrow_attach_handler(u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    return sys_burrow_attach_for_proc(t->proc, length_raw);
}

// The shared argument gate for both detach entries (#199 factored it out): the
// alignment/rounding rules and the window confinement are ONE set of rules, not
// two. Writes *length_out only on success (0).
static s64 detach_args_check(struct Proc *p, u64 vaddr_raw, u64 length_raw,
                             u64 *length_out) {
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

    *length_out = length;
    return 0;
}

// The per-VMA detach body (#199 factored it from sys_burrow_detach_for_proc,
// byte-identical semantics): exact-match [vaddr_raw, vaddr_raw+length) against
// ONE installed VMA, remove it, settle the I-32 accounting. Caller holds
// as->lock and has already run detach_args_check.
//
// D-3c F1: `*out_free` receives the Burrow whose mapping-drop was the last ref
// (or NULL) -- the caller pushes it onto a local stack and frees it with
// burrow_free_deferred AFTER dropping as->lock, because a FILE Burrow's free
// reaches a possibly-sleeping spoor_clunk and a sleeping free under a spinlock
// is the lock-across-sleep extinction. The I-32 uncharge stays here (under the
// lock); only the physical free is deferred.
static s64 detach_one_locked(struct Proc *p, u64 vaddr_raw, u64 length,
                             struct Burrow **out_free) {
    // D-3c re-audit F6: this ALWAYS runs under as->lock, so an inline
    // (possibly-sleeping FILE) free here is the lock-across-sleep extinction.
    // out_free is therefore MANDATORY -- the helper always DEFERS the physical
    // free to the caller. A NULL out_free would silently reintroduce F1, so fail
    // loud rather than take the inline-free arm.
    if (!out_free) extinction("detach_one_locked without out_free (would free under as->lock)");
    *out_free = NULL;
    // #65 (I-32): the uncharge must MATCH the charge. An EAGER attach charged
    // length/PAGE_SIZE at attach; a LAZY attach (SYS_BURROW_ATTACH_LAZY) charged only
    // the FAULTED-in pages (per-page, at fault time -- ARCH §6.5 overcommit). Read
    // the VMA type + resident count BEFORE burrow_unmap frees the VMA/Burrow; under
    // vma_lock so the count is stable. (For a wrong-base/length detach, burrow_unmap
    // returns -1 and the uncharge is skipped.)
    struct Vma *dvma = vma_lookup(p, vaddr_raw);

    // I-42 (CL-7k self-audit): a CODE alias is NOT detachable here. A code
    // region is a PAIR of aliases over one charge, and this syscall has no
    // concept of the pair, so it gets both halves of that wrong:
    //
    //   - ACCOUNTING (the I-32 defeat). Create charges npages ONCE for the
    //     region. Detaching the exec alias uncharges npages and detaching the
    //     writer alias uncharges npages AGAIN -- one charge, two refunds. The
    //     clamp in proc_page_uncharge stops the wrap but not the drift: a
    //     CAP_JIT holder could loop create-then-detach-both, driving its
    //     page_count to 0 while its real usage never changed, and then allocate
    //     a full PROC_PAGE_MAX again. That defeats the per-Proc bound for
    //     exactly the class of Proc (a JIT-capable app) it exists to bound.
    //
    //   - LIFETIME. Detaching one alias leaves the other orphaned: its peer is
    //     gone, so SYS_JIT_DESTROY refuses it and the region survives until
    //     Proc exit with no way to release it.
    //
    // Refusing is the right fix rather than teaching detach to find the peer:
    // the JIT syscalls own this lifetime, and one condition here keeps that
    // ownership total. Self-inflicted either way -- a Proc can only do this to
    // its own region -- but a bound that a capability holder can zero is not a
    // bound.
    if (dvma && dvma->burrow && dvma->burrow->magic == VMO_MAGIC &&
        dvma->burrow->type == BURROW_TYPE_CODE) {
        // Return WITH the lock held -- the caller owns the lock pair (#199
        // factoring; the pre-factor body unlocked here, and leaving that in
        // made the wrapper's unlock a preempt-underflow double).
        return -1;
    }

    // #122: a POSITIVE allowlist -- uncharge page_count only for the two VMA
    // shapes that ever CHARGED it. The previous shape was "everything except
    // ANON_LAZY gets length / PAGE_SIZE", which refunded page_count for two
    // reachable classes that were charged somewhere else, or nowhere:
    //
    //   - SHARED_IN (SYS_WEFT_MAP). burrow_share_into charges the CLIENT's
    //     shared_map_pages and deliberately leaves page_count alone -- "the
    //     pages are the SHARER's commit". It places the VMA with vma_find_gap
    //     in the burrow-attach window, i.e. inside THIS syscall's range, and a
    //     shared Burrow's type is ANON (or weave-DMA), so it landed squarely in
    //     the eager default. burrow_unmap below already refunds shared_map_pages
    //     off the SHARED_IN flag, so the page_count refund on top was pure drift.
    //   - MMIO / DMA. Both take a CALLER-SUPPLIED vaddr, so a CAP_HW_CREATE
    //     driver can place one inside the window and then detach it. Neither
    //     ever charged page_count.
    //
    // proc_page_uncharge clamps at 0, so this never wrapped -- but it is the
    // same "drift, not wrap" shape as the CODE alias refused above, and the
    // same conclusion applies: a bound a Proc can drive to zero while its real
    // occupancy is unchanged is not a bound. Listing what DID charge (rather
    // than subtracting what didn't) also means a future Burrow type is
    // uncharged by default -- the fail-safe direction.
    // #130: SPLIT the two shapes, because the event that ends the charge is
    // different for each.
    //
    //   - ANON_LAZY charged per FAULT, so the refund is the resident count --
    //     read BEFORE the unmap (the pages are gone after) and applied on a
    //     successful unmap. A lazy Burrow has no second owner (it cannot be
    //     Loom-registered -- loom_resolve_buf requires BURROW_TYPE_ANON -- and
    //     cannot be Weft-shared), so unmapping it always frees it.
    //
    //   - ANON charged the whole buddy-rounded occupancy ONCE, and its pages
    //     can outlive the VMA: a Loom ring, a Loom registered buffer, and a
    //     Weft share each hold a handle_count ref. So the refund is applied iff
    //     the unmap was the drop that actually FREED the pages -- reported by
    //     burrow_unmap_reporting rather than predicted from the type.
    //
    // The predicted form is what #106-F1 got wrong twice over. Refunding on the
    // TYPE let EL0 detach a Loom ring, take the refund, keep the pages on the
    // Loom's ref, and re-attach a full PROC_PAGE_MAX -- an unprivileged ~1.5x
    // breach of the floor. Then refunding on a handle_count SAMPLED BEFORE THE
    // DROP swung it the other way: on the ordinary teardown order (detach, then
    // close -- what Ring::drop does) the sample reads 1, the refund is skipped,
    // and loom_free went on to free the pages with nothing uncharging -- a
    // PERMANENT over-charge, and a registered buffer can be the entire budget.
    // Both directions come from treating "the mapping went away" as if it were
    // "the pages went away". They are separate events; only the drop itself
    // knows which one happened.
    //
    // #131 amends the eager arm: `freed` is a SUFFICIENT release condition (if
    // nothing at all still holds the region, this Proc certainly does not), but
    // it is not a NECESSARY one. When the region survives because it was shared
    // into another Proc, this Proc has walked away from pages it can no longer
    // reach -- charging it for them caps it for nothing, and nothing downstream
    // can settle the charge either, because the last drop is then the CONSUMER's
    // vma_drain: generic code, in another Proc, holding that Proc's vma_lock,
    // with no way to name the payer. That is the shape netd hits on every closed
    // zero-copy flow (it detaches its ring at slot_unref while the guest's
    // mapping and the binding pin live on), and it leaked 64 pages a flow.
    //
    // shared_out is the discriminator, and it must be shared_out rather than
    // "does anything else still hold this": the Proc's OWN other claim (a Loom
    // registered-buffer pin on its own buffer) also keeps the region alive, and
    // there the charge must STAY until that claim drops.
    // `paid` replaces the old eager_anon boolean outright: a nonzero claim IS
    // "this Proc is the recorded payer for this eager region", which is strictly
    // narrower than "this is an eager ANON VMA". An eager ANON region that was
    // never charged (nothing recorded a payer) now refunds nothing instead of
    // the recomputed occupancy -- the #122 rule, enforced by attribution rather
    // than by enumerating shapes.
    u32 lazy_uncharge = 0;
    bool shared_out   = false;
    u32  paid         = 0;
    // Snapshot the BURROW pointer, not the VMA: burrow_unmap_reporting frees the
    // Vma struct, so `dvma` is dangling the moment it returns. The Burrow itself
    // survives whenever the drop did not free it -- which is exactly the case
    // where anything below still needs it.
    struct Burrow *dv = NULL;
    if (dvma && dvma->burrow && dvma->burrow->magic == VMO_MAGIC &&
        !(dvma->flags & VMA_FLAG_SHARED_IN)) {
        dv = dvma->burrow;
        if (dv->type == BURROW_TYPE_ANON_LAZY)
            lazy_uncharge = burrow_lazy_resident_count(dv);
        else if (dv->type == BURROW_TYPE_ANON) {
            shared_out = burrow_is_shared_out(dv);
            // Claim BEFORE the drop: a freeing drop takes the record with it.
            // Returns 0 unless this Proc is the recorded payer -- which is what
            // keeps a consumer from ever refunding the sharer's charge.
            paid = burrow_charge_claim(dv, p);
        }
    }

    // burrow_unmap exact-matches [vaddr, vaddr + length) against an
    // installed VMA (no partial detach at v1.0), removes it, and frees
    // the Burrow's pages -- for an ANON_LAZY Burrow that is the resident
    // sparse slots (burrow_free_internal's ANON_LAZY arm).
    bool freed = false;
    // D-3c F1: hand `out_free` down so the FREE is deferred past as->lock. The
    // dead Burrow (if any) rides back to the caller, which frees it after the
    // unlock. `dv` here (the pre-drop snapshot) is only touched below for the
    // charge-restore path, which by construction runs when the Burrow SURVIVED.
    struct Burrow *tf = NULL;
    int rc = burrow_unmap_reporting(p, vaddr_raw, length, &freed, &tf);
    *out_free = tf;
    if (rc == 0 && lazy_uncharge)
        proc_page_uncharge(p, lazy_uncharge);
    if (paid) {
        // The refund is the RECORDED charge, not a recomputation: the record is
        // what the attach actually billed, so the two cannot drift even if this
        // path's view of `length` ever did.
        if (rc == 0 && (freed || shared_out))
            proc_page_uncharge(p, paid);
        else
            // Either the detach failed (nothing dropped) or the region survives
            // on one of THIS Proc's own remaining claims -- a Loom registered
            // buffer being the only one that exists. Put the claim back so that
            // claim's own drop settles it. `dv` is live in both cases:
            // !freed means something still holds it, and rc != 0 means nothing
            // was dropped at all.
            burrow_charge_restore(dv, p, paid);
    }

    return (s64)rc;
}

s64 sys_burrow_detach_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw) {
    u64 length;
    if (detach_args_check(p, vaddr_raw, length_raw, &length) != 0)
        return -1;
    struct Burrow *to_free = NULL;
    spin_lock(&p->as->lock);
    s64 rc = detach_one_locked(p, vaddr_raw, length, &to_free);
    spin_unlock(&p->as->lock);
    // D-3c F1: free OUTSIDE as->lock -- a FILE Burrow's free reaches a
    // possibly-sleeping spoor_clunk (a 9P Tclunk), and sleeping under a plain
    // spinlock is the lock-across-sleep extinction.
    if (to_free) burrow_free_deferred(to_free);
    return rc;
}

// #199: the RANGE detach the phenotype munmap row needs -- D-3b's MAP_FIXED
// split turns one library map into 2-3 VMAs, and musl's unmap_library then
// munmaps the WHOLE span in one call (its error path and dlclose both do), so
// an exact-match-only munmap leaks the entire library. Linux semantics over
// the D-3 shapes: every VMA WHOLLY inside [vaddr, vaddr+len) is detached (each
// one whole -- this is NOT partial unmap), holes are fine, an empty range
// succeeds. Refused whole -- nothing detached -- when any VMA straddles a
// boundary (true partial unmap, post-v1.0) or a CODE-alias region is inside
// (the I-42 pair-lifetime rule detach_one_locked enforces; refusing UP FRONT
// keeps the range atomic instead of stopping half-torn-down).
//
// NATIVE SYS_BURROW_DETACH deliberately keeps exact-match: this widening is a
// LINUX semantic, and the native ABI does not change under a phenotype chunk.
s64 sys_munmap_range_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw) {
    u64 length;
    if (detach_args_check(p, vaddr_raw, length_raw, &length) != 0)
        return -1;
    u64 end = vaddr_raw + length;

    spin_lock(&p->as->lock);

    // Validation pass -- ALL refusals decided before the first removal, under
    // the same lock hold, so the detach loop below cannot stop midway.
    for (struct Vma *v = vma_next_overlap_in(p->as, vaddr_raw, end); v;
         v = vma_next_overlap_in(p->as, v->vaddr_end, end)) {
        if (v->vaddr_start < vaddr_raw || v->vaddr_end > end) {
            spin_unlock(&p->as->lock);
            return -1;                   // boundary straddle: partial unmap
        }
        if (v->burrow && v->burrow->magic == VMO_MAGIC &&
            v->burrow->type == BURROW_TYPE_CODE) {
            spin_unlock(&p->as->lock);
            return -1;                   // I-42 pair lifetime: JIT syscalls own it
        }
    }

    // Detach loop. Each iteration removes the first remaining VMA whole, via
    // the SAME per-VMA body the exact syscall uses (so the I-32 refund logic
    // exists ONCE). Validation makes a failure unreachable; if one happens
    // anyway, stop rather than spin -- the guard is against an infinite loop,
    // not a real path.
    //
    // D-3c F1: the sleeping burrow frees are DEFERRED so the whole range removes
    // under ONE continuous as->lock hold (the atomicity the straddle-refusal
    // validation depends on), while the possibly-sleeping spoor_clunk frees run
    // AFTER the unlock. Each detach hands back its dead Burrow (if any); they
    // stack via deferred_free_next -- an uncapped chain that needs no allocation
    // and no lock (each Burrow is at {0,0}, unreachable by any other path).
    struct Burrow *dead = NULL;
    struct Vma *v;
    while ((v = vma_next_overlap_in(p->as, vaddr_raw, end)) != NULL) {
        struct Burrow *tf = NULL;
        if (detach_one_locked(p, v->vaddr_start,
                              v->vaddr_end - v->vaddr_start, &tf) != 0) {
            spin_unlock(&p->as->lock);
            // Free what we already collected before returning the error --
            // those VMAs are gone; leaking their Burrows would be worse.
            while (dead) { struct Burrow *n = dead->deferred_free_next;
                           dead->deferred_free_next = NULL;
                           burrow_free_deferred(dead); dead = n; }
            return -1;
        }
        if (tf) { tf->deferred_free_next = dead; dead = tf; }
    }
    spin_unlock(&p->as->lock);
    // The sleeping frees, now with no lock held.
    while (dead) { struct Burrow *n = dead->deferred_free_next;
                   dead->deferred_free_next = NULL;
                   burrow_free_deferred(dead); dead = n; }
    return 0;                            // incl. the nothing-mapped no-op (Linux)
}

static s64 sys_burrow_detach_handler(u64 vaddr_raw, u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    return sys_burrow_detach_for_proc(t->proc, vaddr_raw, length_raw);
}

// =============================================================================
// Overcommit / I-32 (ARCH §6.5 "The overcommit model"): the demand-zero lazy attach
// + decommit. The eager SYS_BURROW_ATTACH / SYS_BURROW_DETACH above are byte-
// unchanged; these are additive (SYS_BURROW_ATTACH_LAZY = 83 / SYS_BURROW_DECOMMIT =
// 84). The contract reaches every program through the two malloc substrates
// (libthyla-rs sysAlloc + the pouch boundary-line mmap) + the Go runtime's
// sysReserve/sysUnused (#321).
// =============================================================================

// SYS_BURROW_ATTACH_LAZY: the demand-zero twin of sys_burrow_attach_for_proc. Same
// window placement + VMA install, but (a) the Burrow is BURROW_TYPE_ANON_LAZY (no
// eager pages), and (b) the I-32 page_count is NOT charged here -- it is charged per
// page at FAULT time (the whole point of a free reservation, so page_count tracks
// true RSS). The VMA-count axis (PROC_VMA_MAX) IS charged inside vma_insert, so a
// free lazy reservation cannot exhaust the vma slab.
s64 sys_burrow_attach_lazy_for_proc(struct Proc *p, u64 length_raw) {
    if (!p)                                          return -1;
    if (length_raw == 0)                             return -1;
    // BURROW_RESERVE_MAX (1 GiB), NOT BURROW_ATTACH_MAX (256 MiB): a lazy reservation
    // commits no data pages, so the eager-sized bound would defeat the purpose
    // (audit F1; Go-stock reserves a ~512-MiB page-summary). page_count (at fault) +
    // PROC_VMA_MAX bound the real resource use.
    if (length_raw > BURROW_RESERVE_MAX)             return -1;

    u64 length = (length_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);

    spin_lock(&p->as->lock);

    u64 vaddr;
    if (vma_find_gap(p, length, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &vaddr) != 0) {
        spin_unlock(&p->as->lock);
        return -1;
    }

    // No proc_page_charge here -- lazy reservations commit (and charge) per page at
    // fault time. The VMA-count cap (vma_insert -> proc_vma_charge) is the bound on
    // the free reservation; a non-TCB Proc at PROC_VMA_MAX fails burrow_map below.
    struct Burrow *b = burrow_create_anon_lazy(length);
    if (!b) {
        spin_unlock(&p->as->lock);
        return -1;
    }

    // burrow_map installs the VMA (vma_alloc -> burrow_acquire_mapping; mapping_count
    // -> 1; vma_insert -> proc_vma_charge). Then drop the construction handle:
    // handle_count -> 0, mapping_count = 1 keeps the Burrow alive (Tier 1). On
    // burrow_map failure (overlap / vma-cap / OOM) the construction handle is the
    // only ref; burrow_unref frees the empty lazy Burrow (no pages committed).
    if (burrow_map(p, b, vaddr, length, VMA_PROT_RW) != 0) {
        // The construction handle is the only ref; freeing an EMPTY anon-lazy
        // Burrow never sleeps (no Spoor to clunk, no pages committed), so this
        // one may stay under the lock.
        burrow_unref(b);
        spin_unlock(&p->as->lock);
        return -1;
    }
    spin_unlock(&p->as->lock);
    // #193: the success-path drop sits OUTSIDE as->lock to match the FILE arm --
    // an anon-lazy free never sleeps today, but two success paths should not
    // need two different safety arguments.
    burrow_unref(b);
    return (s64)vaddr;
}

// DISTRO D-3: the FILE mmap arm. A read-only / executable file-backed
// MAP_PRIVATE mapping, demand-paged through the SAME BURROW_TYPE_FILE machinery
// exec has used since REVENANT -- and through the same qid-keyed Image cache, so
// one copy of a library's text serves every container that maps it. This is the
// arc's headline composition: nothing new pages the file in, only a new door to
// the machinery that already did.
//
// The I-36 conditions are re-satisfied here rather than inherited, because this
// is a NEW entry to the fault arm and I-36's premise ("kernel-internal") is what
// D-3 relaxes. Taking them in order: (1) install-once is filepages[]'s, unchanged;
// (2) the page-in is death-interruptible by inheritance from dev9p's read, and
// reachable from mmap now rather than only from exec -- the same unwind either
// way, since the fault arm does not know which entry created the Burrow;
// (3) fail-close on I/O error is file_demand_page_single's FAULT_USER_BUS;
// (4) W^X holds because PROT_WRITE cannot reach here (vivarium_mmap_file_decide
// refuses it) and vma_alloc rejects W+X independently; (5) I-cache sync is the
// fault arm's, keyed on the VMA's own X bit; (6) Image-cache eviction safety is
// unchanged; (7) pin-at-map replaces pin-at-exec -- the Burrow adopts a Spoor ref
// for its whole life, so the guest closing the fd cannot pull the backing file
// out from under a resident mapping.
//
// I-32: the demand-paged FILE pages keep the R-5 uncharged-at-v1.0 posture (they
// are shared, so charging them per-mapper would count one physical page N times);
// the VMA-count axis IS charged inside vma_insert. The `filepages` array is an
// uncharged kernel allocation proportional to `length` -- task #191, which is the
// PRE-EXISTING lazy-anon hazard this arm sits beside rather than widens: the cap
// below is BURROW_ATTACH_MAX (256 MiB), 4x TIGHTER than the lazy path's
// BURROW_RESERVE_MAX. Measured, the largest library in a stock Alpine rootfs is
// libcrypto.so.3 at ~4.2 MiB of map span, so the cap is ~60x real need.
//
// exec_map_vouched (#217) -- may this file's bytes become EXECUTABLE pages?
//
// The one place the MNOEXEC rule is spelled, called by every site that turns a
// file into executable pages: both phenotype mmap arms and exec's own name
// resolution. It deliberately does NOT live inside image_lookup_or_create, the
// shared chokepoint both mmap arms funnel through -- that cache is keyed on
// (spoor identity, offset, length, exec) and NOT on the Territory, so a hit
// seeded by a Proc that may exec-map would hand the same executable Burrow to a
// Proc whose namespace forbids it. The verdict is per-namespace; the cache is
// global; so the check belongs strictly before the cache, in the caller.
static bool exec_map_vouched(struct Proc *p, const struct Spoor *sp) {
    if (!p || !sp || !sp->dev) return false;
    // THE FLOOR (#217 F1). Only a Dev that serves real file content may back
    // executable pages at all. This is NOT redundant with the mount check below
    // -- it is what catches the class the mount check structurally cannot:
    // devenv stamps the CALLING Proc's env devno at walk time, so a container's
    // /env files never share (dc, devno) with the /env mount source viv
    // installed, and no MNOEXEC flag can ever cover them. An environment
    // variable is not code; nor is a /proc field, a /srv endpoint or a console.
    if (!sp->dev->may_back_exec) return false;
    // THE REFINEMENT. Among Devs that may, a specific mount can still be marked
    // MNOEXEC by whoever composed the namespace.
    return !mount_noexec_covers(p->territory, sp->dc, sp->devno);
}

// Returns the mapped VA, or a negated T_E_* the caller passes straight out.
s64 sys_mmap_file_for_proc(struct Proc *p, u64 fd_raw, u64 length_raw,
                           bool exec, u64 offset) {
    if (!p)                                          return -(s64)T_E_INVAL;
    if (length_raw == 0)                             return -(s64)T_E_INVAL;
    if (length_raw > BURROW_ATTACH_MAX)              return -(s64)T_E_NOMEM;
    if (offset & (u64)(PAGE_SIZE - 1))               return -(s64)T_E_INVAL;

    u64 length = (length_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);
    // A mapping whose file window would wrap past the end of the address space
    // cannot be described; refuse before it reaches the Burrow's own guards.
    if (offset + length < offset)                    return -(s64)T_E_INVAL;

    // T_RIGHT_READ, not T_RIGHT_EXEC, and deliberately: there is no per-handle
    // execute right on a Spoor to demand, so requiring one would fail every
    // mapping. READ is the right authority for the BYTES.
    //
    // What READ alone does not authorize is turning those bytes into CODE, and
    // this comment used to claim otherwise -- "exactly the authority whose
    // result the guest can already obtain with pread(2), so the mapping grants
    // nothing new". That is false in the case that matters: pread yields the
    // bytes as DATA, this yields them as an executable page, and exec (the
    // other way to run them) walks OEXEC and REFUSES a file carrying no X bit.
    // The mapping grants authority exec withholds. What closes the gap is the
    // MNOEXEC vouching below (#217, ARCH 6.5); the wording is corrected here
    // rather than left standing as the in-code twin of a premise the D-close
    // arc round already falsified in scripture.
    // #844: the ref is TRANSFERRED to us -- spoor_clunk on every path below.
    struct Spoor *sp = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_READ);
    if (!sp)                                         return -(s64)T_E_BADF;

    // A PLAIN FILE only. A directory has no byte stream to page from; a symlink
    // must be followed, not mapped (the #184 discipline, where the twin's
    // "a symlink fid is not byte-I/O-able" safety argument turned out FALSE
    // against Stratum's h_lopen -- so this gate is explicit rather than assumed);
    // an append-only file's byte positions are not stable under a concurrent
    // writer, which is precisely what a demand-paged mapping assumes.
    if (sp->qid.type & (QTDIR | QTSYMLINK | QTAPPEND)) {
        spoor_clunk(sp);
        return -(s64)T_E_INVAL;
    }
    // #81: an O_PATH handle is a NAVIGATION capability that deliberately carries
    // no byte-I/O authority. Mapping through one would be byte I/O by another
    // name -- the exact bypass CWALKONLY exists to prevent.
    if (sp->flag & CWALKONLY) {
        spoor_clunk(sp);
        return -(s64)T_E_INVAL;
    }
    // #217: the MNOEXEC vouching. Only the EXEC request is gated -- a read-only
    // file mapping off a noexec mount stays legal, exactly as on Linux, because
    // noexec restricts what may become code and not what may be read. T_E_PERM
    // matches what Linux answers for the same refusal (path_noexec + VM_EXEC).
    if (exec && !exec_map_vouched(p, sp)) {
        spoor_clunk(sp);
        return -(s64)T_E_PERM;
    }
    // Positional reads are the whole mechanism: every page-in is a read at an
    // explicit offset. A Dev with no read slot cannot serve one, and the fault
    // arm's answer would be a snare:bus at first touch -- report it now, where
    // the guest gets an errno instead of a fault.
    if (!sp->dev || !sp->dev->read) {
        spoor_clunk(sp);
        return -(s64)T_E_INVAL;
    }

    // #194 (I-32): the backing size is REQUIRED on this guest-facing arm -- it
    // is what lets the fault arm refuse pages wholly past EOF (Linux SIGBUS
    // semantics) instead of minting uncharged demand-zero memory bounded only
    // by BURROW_ATTACH_MAX x PROC_VMA_MAX. A Dev that cannot answer -- or a
    // hostile near-2^64 size, which the predicate also excludes -- is refused
    // outright; only exec (kernel-driven, over the immutable baked ramfs) may
    // admit an unknown size, and that policy lives at ITS call site.
    u64 file_limit = spoor_file_size(sp);
    if (!burrow_file_limit_known(file_limit)) {
        spoor_clunk(sp);
        return -(s64)T_E_IO;
    }

    // image_lookup_or_create CONSUMES one Spoor ref on every success path
    // (adopted into the new Burrow on a miss, clunk'd as redundant on a hit) and
    // consumes NOTHING on a NULL return. Hand it a FRESH ref so our own stays
    // ours to drop, exactly as map_file_backed does.
    spoor_ref(sp);
    struct Burrow *b = image_lookup_or_create(sp, offset, (size_t)length, exec,
                                              file_limit);
    if (!b) {
        spoor_clunk(sp);                 // the fresh ref it did not consume
        spoor_clunk(sp);                 // ours
        return -(s64)T_E_NOMEM;
    }
    spoor_clunk(sp);                     // ours; the Burrow holds the pin now

    // R for rodata, R+X for text. Never writable -- the decide arm refused
    // PROT_WRITE, and vma_alloc would reject W+X anyway (I-12, two independent
    // gates).
    u32 prot = exec ? (u32)(VMA_PROT_READ | VMA_PROT_EXEC) : (u32)VMA_PROT_READ;

    spin_lock(&p->as->lock);
    u64 vaddr;
    if (vma_find_gap(p, length, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &vaddr) != 0) {
        spin_unlock(&p->as->lock);
        burrow_unref(b);                 // the cache's ref keeps it cached
        return -(s64)T_E_NOMEM;
    }
    if (burrow_map(p, b, vaddr, length, prot) != 0) {
        spin_unlock(&p->as->lock);
        burrow_unref(b);
        return -(s64)T_E_NOMEM;
    }
    spin_unlock(&p->as->lock);
    // Drop the construction handle OUTSIDE as->lock (#193): mapping_count + the
    // cache's own ref keep the image alive (I-7 dual count), so this is never
    // the last ref today -- but if it ever were, burrow_free_internal ->
    // spoor_clunk may sleep, the same leaf-lock rule file_demand_page_slow
    // states. Dropping after the unlock removes the need for that non-local
    // argument entirely; the failure paths above already did.
    burrow_unref(b);

    // A user VA is below 2^47, so this can never be mistaken for the [-4095,-1]
    // errno band a Linux caller checks.
    return (s64)vaddr;
}

// DISTRO D-3b: the shared front half of both MAP_FIXED arms -- validate the
// window and convert the Linux prot word to VMA_PROT_*. Split out because the
// two arms differ only in where the backing comes from, and duplicating the
// bounds would be exactly the "the fix that exists on site N stops you asking
// about site N+1" trap. `*prot_out` is written only on success.
static s64 mmap_fixed_window(u64 addr, u64 length_raw, u32 pr,
                             u64 *length_out, u32 *prot_out) {
    if (length_raw == 0)                             return -(s64)T_E_INVAL;
    if (length_raw > BURROW_ATTACH_MAX)              return -(s64)T_E_NOMEM;
    if (addr == 0 || (addr & (u64)(PAGE_SIZE - 1)))  return -(s64)T_E_INVAL;

    u64 length = (length_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);
    if (addr + length < addr)                        return -(s64)T_E_INVAL;

    u32 prot = 0;
    if (pr & VIV_PROT_READ)  prot |= (u32)VMA_PROT_READ;
    if (pr & VIV_PROT_WRITE) prot |= (u32)VMA_PROT_WRITE;
    if (pr & VIV_PROT_EXEC)  prot |= (u32)VMA_PROT_EXEC;

    *length_out = length;
    *prot_out   = prot;
    return 0;
}

// DISTRO D-3b: the EAGER PRIVATE COPY -- a writable MAP_FIXED file window served
// as anonymous memory pre-filled from the file. The alternative (copy-on-write
// over the shared Image-cache pages) is rejected in the header: it would make
// one container's writes a correctness question about another container's view
// of the same library.
//
// Charged, unlike the demand-paged read-only arm (task #194): burrow_lazy_populate
// takes the I-32 page charge for the whole run up front, so an over-cap request
// is refused whole rather than half-populated, and a guest cannot mint free
// pages here by naming a huge length.
//
// A SHORT READ IS NOT AN ERROR. The window legitimately extends past the file's
// data -- arm 2's length comes from p_memsz, not p_filesz -- and those pages must
// read as zero, which is exactly what the untouched KP_ZERO slots already are.
// Only a NEGATIVE read (I/O error, or a death-interruptible unwind) fails.
//
// Give back a populate's I-32 page charge and drop the Burrow. The exec twin is
// lazy_populate_unwind (kernel/exec.c); the pairing lives beside the populate
// rather than in each caller for the same #106/#130 reason it does there.
// `exempt` is deliberately absent: addrspace_charge_pages COUNTS regardless of
// exemption (exemption bypasses the CAP, not the accounting), so the uncharge
// must be unconditional to stay paired.
static void mmap_eager_unwind(struct AddrSpace *as, struct Burrow *b,
                              size_t npages) {
    if (npages > 0) {
        spin_lock(&as->lock);            // the stated precondition of the counter ops
        addrspace_uncharge_pages(as, (u32)npages);
        spin_unlock(&as->lock);
    }
    burrow_unref(b);
}

// THE CHARGE IS THE CALLER'S THE MOMENT POPULATE SUCCEEDS (#197, the L-7 F4
// contract at a new call site). burrow_lazy_populate charges the whole run
// against `as` on success and NOTHING gives it back later: the pages go with the
// Burrow's last unref, but the counter does not -- there is no uncharge anywhere
// in the Burrow free path (grep addrspace_uncharge_pages: only populate's own
// failure arm and burrow_decommit). So every failure PAST the populate has to
// unwind the counter by hand, exactly as exec's lazy_populate_unwind does, or
// `as->page_count` stops meaning true RSS (ARCH section 6.5).
//
// Returns the Burrow with ONE handle ref for the caller, or NULL. Does not
// consume `sp`. On NULL, the charge has been fully returned.
#ifdef KERNEL_TESTS
// #197's regression drives this directly: the leak lives entirely inside it,
// and reaching it through the syscall shell would need a handle-table fixture
// that tests the lookup rather than the charge pairing.
struct Burrow *mmap_eager_copy_for_test(struct AddrSpace *as, bool exempt,
                                        struct Spoor *sp, u64 offset, u64 length);
#endif

static struct Burrow *mmap_eager_copy(struct AddrSpace *as, bool exempt,
                                      struct Spoor *sp, u64 offset, u64 length) {
    struct Burrow *b = burrow_create_anon_lazy((size_t)length);
    if (!b) return NULL;

    size_t npages = (size_t)(length / PAGE_SIZE);
    if (burrow_lazy_populate(as, exempt, b, 0, npages) != 0) {
        burrow_unref(b);                 // populate self-unwinds its own charge
        return NULL;
    }

    // burrow_lazy_slot_kva's precondition holds: this Burrow is not mapped into
    // any address space and is unreachable from a second thread until the caller
    // maps it, so the raw slot pointer cannot be freed under us.
    for (size_t slot = 0; slot < npages; slot++) {
        u8 *kva = (u8 *)burrow_lazy_slot_kva(b, slot);
        if (!kva) { mmap_eager_unwind(as, b, npages); return NULL; }
        size_t got = 0;
        while (got < (size_t)PAGE_SIZE) {
            long n = sp->dev->read(sp, kva + got, (long)(PAGE_SIZE - got),
                                   (s64)(offset + slot * PAGE_SIZE + got));
            // The reachable failure: a 9P I/O error, or a death-interruptible
            // unwind partway through a multi-page copy.
            if (n < 0) { mmap_eager_unwind(as, b, npages); return NULL; }
            if (n == 0) goto eof;                          // past the data
            got += (size_t)n;
        }
    }
eof:
    return b;
}

#ifdef KERNEL_TESTS
struct Burrow *mmap_eager_copy_for_test(struct AddrSpace *as, bool exempt,
                                        struct Spoor *sp, u64 offset, u64 length) {
    return mmap_eager_copy(as, exempt, sp, offset, length);
}
#endif

// DISTRO D-3b arm 2 (dynlink.c:842): a fd-backed MAP_FIXED overlay. Non-writable
// windows ride the shared Image cache exactly as D-3a does; a writable one gets
// the eager private copy above. Returns `addr` on success (MAP_FIXED's contract:
// the requested address or nothing).
s64 sys_mmap_fixed_file_for_proc(struct Proc *p, u64 addr, u64 fd_raw,
                                 u64 length_raw, u32 pr, u64 offset) {
    if (!p)                                          return -(s64)T_E_INVAL;
    if (offset & (u64)(PAGE_SIZE - 1))               return -(s64)T_E_INVAL;

    u64 length; u32 prot;
    s64 werr = mmap_fixed_window(addr, length_raw, pr, &length, &prot);
    if (werr != 0)                                   return werr;
    if (offset + length < offset)                    return -(s64)T_E_INVAL;

    // The same gates as the D-3a arm, and for the same reasons: READ is the
    // authority a mapping consumes; a directory has no byte stream; a symlink
    // must be followed (#184); an append-only file has no stable byte positions;
    // an O_PATH handle carries no byte-I/O authority (#81); a Dev with no read
    // slot cannot serve a page-in.
    struct Spoor *sp = sys_lookup_spoor(p, (hidx_t)fd_raw, RIGHT_READ);
    if (!sp)                                         return -(s64)T_E_BADF;
    if ((sp->qid.type & (QTDIR | QTSYMLINK | QTAPPEND)) ||
        (sp->flag & CWALKONLY) || !sp->dev || !sp->dev->read) {
        spoor_clunk(sp);
        return -(s64)T_E_INVAL;
    }
    // #217: the same MNOEXEC vouching as the non-fixed arm, and placed BEFORE
    // the writable/demand-paged split rather than inside the demand-paged
    // branch that is its only exec-capable producer today. Sited by prot alone,
    // it cannot be walked past by a future branch: an arm added below inherits
    // the gate instead of needing to remember it. (W+X cannot arrive here --
    // vma_alloc refuses it -- so testing EXEC before the split loses nothing.)
    if ((prot & (u32)VMA_PROT_EXEC) && !exec_map_vouched(p, sp)) {
        spoor_clunk(sp);
        return -(s64)T_E_PERM;
    }

    bool writable = (prot & (u32)VMA_PROT_WRITE) != 0;
    struct Burrow *b;
    if (writable) {
        b = mmap_eager_copy(p->as, proc_resource_exempt(p), sp, offset, length);
        spoor_clunk(sp);                 // the copy is done; nothing pins the file
        if (!b)                                      return -(s64)T_E_NOMEM;
    } else {
        // #194: same guest-facing fail-closed rule as the non-fixed FILE arm --
        // a demand-paged mapping needs the backing size or it can mint
        // uncharged demand-zero pages past EOF. (The writable branch above
        // needs none: its eager copy is CHARGED whole by populate, and a short
        // read legitimately zero-fills.) NOTE this branch has no producer on
        // the measured rootfs (#195: every arm-2 request is RW), so only unit
        // tests and a future -z separate-code toolchain reach it.
        u64 fx_limit = spoor_file_size(sp);
        if (!burrow_file_limit_known(fx_limit)) {
            spoor_clunk(sp);
            return -(s64)T_E_IO;
        }
        // image_lookup_or_create CONSUMES a Spoor ref on success and none on
        // NULL, so hand it a fresh one and keep ours to drop.
        spoor_ref(sp);
        b = image_lookup_or_create(sp, offset, (size_t)length,
                                   (prot & (u32)VMA_PROT_EXEC) != 0, fx_limit);
        if (!b) {
            spoor_clunk(sp);             // the fresh ref it did not consume
            spoor_clunk(sp);             // ours
            return -(s64)T_E_NOMEM;
        }
        spoor_clunk(sp);                 // ours; the Burrow holds the pin now
    }

    // D-3c re-audit F5 [P1]: a MAP_FIXED exact-cover REPLACES an existing VMA,
    // whose Burrow may be a 9P-backed FILE image (a bypassed mmap at {h:0,m:1})
    // whose free reaches a possibly-sleeping spoor_clunk. burrow_map_fixed hands
    // that dead Burrow back via `fx_free` instead of freeing it under as->lock;
    // we free it here, past the unlock (the F1 deferred-free pattern).
    struct Burrow *fx_free = NULL;
    spin_lock(&p->as->lock);
    int rc = burrow_map_fixed(p, b, addr, (size_t)length, prot, /*offset=*/0, &fx_free);
    spin_unlock(&p->as->lock);
    if (fx_free) burrow_free_deferred(fx_free);
    // OUTSIDE the lock, unlike the D-3a arm (task #193): the mapping holds the
    // Burrow alive on success, and on failure this is the last ref and dropping
    // it can reach the allocator.
    //
    // #197: the eager arm's populate charge is OURS until the mapping takes it
    // over. On a map failure the pages go with the unref but the counter would
    // not, so the writable arm unwinds it here; the Image-cache arm populated
    // nothing and owes nothing.
    if (rc != 0 && writable) {
        mmap_eager_unwind(p->as, b, (size_t)(length / PAGE_SIZE));
        return -(s64)T_E_NOMEM;
    }
    burrow_unref(b);
    if (rc != 0)                                     return -(s64)T_E_NOMEM;
    return (s64)addr;
}

// DISTRO D-3b arm 3 (dynlink.c:851): the anonymous MAP_FIXED bss tail. Demand-
// zero, so it charges per page at fault time exactly like every other lazy anon
// mapping.
s64 sys_mmap_fixed_anon_for_proc(struct Proc *p, u64 addr, u64 length_raw,
                                 u32 pr) {
    if (!p)                                          return -(s64)T_E_INVAL;

    u64 length; u32 prot;
    s64 werr = mmap_fixed_window(addr, length_raw, pr, &length, &prot);
    if (werr != 0)                                   return werr;

    struct Burrow *b = burrow_create_anon_lazy((size_t)length);
    if (!b)                                          return -(s64)T_E_NOMEM;

    // D-3c re-audit F5: even the anon arm can EXACT-COVER an existing FILE mapping
    // (the old VMA at `addr` need not be anon), so its replaced Burrow is deferred
    // and freed past the unlock exactly as the file arm does.
    struct Burrow *fx_free = NULL;
    spin_lock(&p->as->lock);
    int rc = burrow_map_fixed(p, b, addr, (size_t)length, prot, /*offset=*/0, &fx_free);
    spin_unlock(&p->as->lock);
    if (fx_free) burrow_free_deferred(fx_free);
    burrow_unref(b);
    if (rc != 0)                                     return -(s64)T_E_NOMEM;
    return (s64)addr;
}

// CL-4: pick the length out of SYS_BURROW_ATTACH_LAZY's two accepted calling
// conventions. Pure + exported so the shape gate is unit-testable without a
// live Proc; the full rationale lives at the dispatch site. Returns 0 -- which
// sys_burrow_attach_lazy_for_proc rejects -- for anything that is neither the
// native 1-arg form nor an anonymous-private Linux mmap, so a file-backed or
// MAP_FIXED request keeps the pre-CL-4 fail-closed -1 instead of silently
// receiving anonymous zero pages (ARCH 6.5: there is no file-backed mmap).
u64 burrow_lazy_len_from_args(u64 x0, u64 x1, u64 flags, u64 fd_raw) {
    if (x0 != 0) return x0;                  // native: length in x0
    // Linux/aarch64 mmap flag encoding -- inherent to a shim whose whole job is
    // to accept the Linux anon-mmap shape.
    enum { LX_MAP_FIXED = 0x10, LX_MAP_ANONYMOUS = 0x20 };
    if (!(flags & LX_MAP_ANONYMOUS)) return 0;   // file-backed -> fail closed
    if (flags & LX_MAP_FIXED)        return 0;   // caller-chosen VA -> refused
    if ((s64)fd_raw >= 0)            return 0;   // anon must carry fd == -1
    return x1;                                   // Linux 6-arg: length in x1
}

static s64 sys_burrow_attach_lazy_handler(u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    return sys_burrow_attach_lazy_for_proc(t->proc, length_raw);
}

// SYS_BURROW_DECOMMIT: release the resident pages of a BURROW_TYPE_ANON_LAZY mapping
// WITHOUT removing the VMA (the madvise(MADV_DONTNEED) analog). Confined to the
// burrow-attach window (the SYS_BURROW_DETACH discipline -- a decommit only makes
// sense on a lazy attach region, all of which live in the window); burrow_decommit
// additionally rejects any non-ANON_LAZY VMA + a range not within one VMA.
s64 sys_burrow_decommit_for_proc(struct Proc *p, u64 vaddr_raw, u64 length_raw) {
    if (!p)                                          return -1;
    if (length_raw == 0)                             return -1;
    // BURROW_RESERVE_MAX, not BURROW_ATTACH_MAX -- a lazy region can be up to the
    // reservation max, so a decommit range may span up to that (audit F1).
    if (length_raw > BURROW_RESERVE_MAX)             return -1;
    if (vaddr_raw & (PAGE_SIZE - 1))                 return -1;

    u64 length = (length_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);

    // Window confinement (matches SYS_BURROW_DETACH): overflow-safe since
    // length <= BURROW_ATTACH_MAX, far below EXEC_USER_BURROW_TOP.
    if (vaddr_raw < EXEC_USER_BURROW_BASE)           return -1;
    if (vaddr_raw > EXEC_USER_BURROW_TOP - length)   return -1;

    spin_lock(&p->as->lock);
    // burrow_decommit does the per-page PTE clear (+ TLBI before free) + page free +
    // page_count uncharge, and rejects a non-ANON_LAZY / out-of-VMA range.
    int rc = burrow_decommit(p, vaddr_raw, length);
    spin_unlock(&p->as->lock);
    return (s64)rc;
}

static s64 sys_burrow_decommit_handler(u64 vaddr_raw, u64 length_raw) {
    struct Thread *t = current_thread();
    if (!t)                                          return -1;
    return sys_burrow_decommit_for_proc(t->proc, vaddr_raw, length_raw);
}

// =============================================================================
// I-42 / CL-7k -- the JIT capability (docs/JIT-ON-WX-DESIGN.md; LLVM-DESIGN.md
// section 8). SYS_JIT_CREATE / SYS_JIT_DESTROY / SYS_ICACHE_SYNC: the only path
// by which bytes a Proc wrote become bytes it can execute.
//
// The mechanism is two aliases of one physical region in ONE Proc -- RW at
// VA_w, RX at VA_x -- so no PTE is ever W AND X and I-12 holds at page
// granularity. Nothing here relaxes the W^X check: each alias is an ordinary
// VMA whose prot make_user_pte_l3 encodes exactly as it does for any other
// mapping. What is new is only that a CODE Burrow is ADMITTED to carry an RX
// alias at all, and that admission is minted by the kernel at create under
// CAP_JIT -- never asserted by the caller at map time (the G-2 WEAVE
// discipline).
//
// This is the kernel's own Lazarus W1.5 alternatives-patcher turned outward:
// that code already writes .text through a transient RW-not-X scratch alias
// while the canonical mapping stays RO+X, and already performs the
// dc cvau / ic ivau / dsb / isb publish sequence. CL-7k exposes the same
// already-trusted mechanism to userspace behind a capability.
// =============================================================================

// The EXEC alias paired with `writer` -- the other VMA of this Proc backed by
// the same CODE Burrow. Unambiguous by construction: a CODE Burrow is created
// with exactly two mappings and is never cross-Proc shareable
// (burrow_share_into admits ANON only), so at most one peer can exist.
//
s64 sys_jit_destroy_for_proc(struct Proc *p, u64 writer_va);

// PRECONDITION: caller holds p->vma_lock (walks p->as->vmas).
static struct Vma *jit_find_peer_locked(struct Proc *p, const struct Vma *writer) {
    for (struct Vma *v = p->as->vmas; v; v = v->next) {
        if (v != writer && v->burrow == writer->burrow)
            return v;
    }
    return NULL;
}

// Is `v` the WRITER alias of a live code region? The writer is the RW alias;
// the exec alias is RX. Both are required to be CODE-backed -- a caller must
// not be able to name an ordinary RW anon mapping and have the JIT paths act
// on it.
static bool jit_vma_is_writer(const struct Vma *v) {
    return v && v->burrow &&
           v->burrow->magic == VMO_MAGIC &&
           v->burrow->type == BURROW_TYPE_CODE &&
           (v->prot & VMA_PROT_WRITE) != 0 &&
           (v->prot & VMA_PROT_EXEC) == 0;
}

// The MECHANISM behind SYS_JIT_CREATE: mint a code region and install BOTH of
// its aliases, returning the pair through kernel pointers.
//
// Split out from the syscall body so the copy-out to userspace is the only
// thing sys_jit_create_for_proc adds. That separation is not cosmetic: it is
// what lets the kernel tests drive the real create path at all. A test runs in
// kproc context, so any out_va it could pass is a KERNEL address, which
// sys_validate_user_buf correctly refuses -- without this split every
// mechanism test would fail on the ABI plumbing and prove nothing about the
// mechanism. (The copy-out arm itself is exercised by the in-guest prover,
// which is a real EL0 Proc with a real user buffer.)
s64 sys_jit_create_region(struct Proc *p, u64 length_raw,
                          u64 *out_writer, u64 *out_exec) {
    if (!p || !out_writer || !out_exec)              return -T_E_INVAL;

    // CAP_JIT is THE gate on this whole surface: it is the authority to make
    // writable bytes executable at all. ACQUIRE load -- proc_become_legate
    // writes caps cross-thread (the A-4a clearance redeem), so a Proc can
    // acquire the cap while another of its threads is running.
    //
    // Checked FIRST, before any argument validation, so that a Proc without
    // the capability learns nothing about which lengths or addresses would
    // have been acceptable (the SYS_CLOCK_SETTIME cap-before-buffer ordering).
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_JIT) == 0)
        return -T_E_ACCES;

    if (length_raw == 0)                             return -T_E_INVAL;
    if (length_raw > JIT_REGION_MAX)                 return -T_E_INVAL;

    // JIT_REGION_MAX is page-aligned, so the rounded length cannot exceed it
    // and the addition cannot overflow.
    u64 length = (length_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);
    // #106: the buddy-rounded occupancy, not the page-rounded request --
    // burrow_create_code below allocates 1 << order like every eager Burrow.
    // JIT_REGION_MAX is 2^14 pages, so a MAX-sized region rounds to itself and
    // the u32 cast is safe; it is the sizes BELOW it that round up (a 33-MiB
    // region occupies 64 MiB), and a JIT emitting odd-sized regions is exactly
    // the workload that makes this routine rather than theoretical.
    u32 npages = (u32)burrow_backing_pages(length);

    spin_lock(&p->as->lock);

    // I-32: charge ONCE for the region, not once per alias. The two aliases are
    // two views of ONE set of physical pages -- charging twice would bill a JIT
    // double for memory it holds once, and the uncharge at destroy would then
    // have to know to refund twice. One region, one charge.
    if (!proc_page_charge(p, npages)) {
        spin_unlock(&p->as->lock);
        return -T_E_NOMEM;
    }

    struct Burrow *b = burrow_create_code(length);
    if (!b) {
        proc_page_uncharge(p, npages);
        spin_unlock(&p->as->lock);
        return -T_E_NOMEM;
    }

    // CL-7k-3 audit F1: invalidate the I-cache over the fresh pages BEFORE any
    // RX PTE can name them.
    //
    // KP_ZERO zeroes MEMORY; it does not touch the instruction cache. Nothing on
    // the free path does either -- burrow_unmap clears PTEs and broadcasts TLBI
    // (a TLB operation), and free_pages does no cache maintenance at all. So a
    // recycled page can still carry I-cache lines holding a PREVIOUS code
    // region's instructions, and a Proc that branches into a page it has not
    // published would fetch them instead of taking the UDF #0 that all-zero
    // memory promises. That promise is stated in four places; this is what makes
    // it true rather than requiring it be weakened.
    //
    // It also restores consistency: every other executable backing in the tree
    // syncs at acquisition for exactly this reason (kernel/exec.c's two eager
    // paths + arch/arm64/fault.c's FILE demand-page arms -- the REVENANT arm's
    // comment names the hazard as "a stale line from a prior occupant of this
    // recycled PA"). Named, not cited by line: #107 moved the exec.c pair.
    // A code Burrow was the sole exception.
    //
    // One call, not a per-page loop: a CODE Burrow is one contiguous
    // alloc_pages chunk, so its direct-map range is contiguous too. Bounded by
    // JIT_REGION_MAX -- the same ceiling the mandatory publish already pays.
    arch_icache_sync_range(pa_to_kva(page_to_pa(b->pages)), length);

    // Both gaps are found and both VMAs installed under ONE lock hold, so a
    // sibling thread cannot claim either gap between them and no observer ever
    // sees a half-installed region. The writer alias is inserted BEFORE the
    // second gap search, so vma_find_gap cannot hand back the range we just
    // took -- the two aliases are necessarily disjoint.
    u64 wva = 0, xva = 0;
    if (vma_find_gap(p, length, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &wva) != 0)
        goto fail_unref;
    if (burrow_map(p, b, wva, length, VMA_PROT_RW) != 0)
        goto fail_unref;

    if (vma_find_gap(p, length, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &xva) != 0)
        goto fail_unmap_writer;
    // VMA_PROT_RX: readable + executable, NOT writable. vma_alloc rejects W|X
    // outright, so this prot could never carry a write bit even by mistake --
    // the W^X guarantee for the exec alias is the same one every other mapping
    // in the system gets, not a special case.
    if (burrow_map(p, b, xva, length, VMA_PROT_RX) != 0)
        goto fail_unmap_writer;

    // Drop the construction handle: the two mappings now own the Burrow
    // (handle_count 0, mapping_count 2). The #847 dual count frees the pages
    // only when BOTH aliases are gone -- which is exactly the lifetime a
    // dual-mapped region needs, with no new refcount to get wrong.
    //
    // #131/#132: record the payer first. A CODE Burrow can reach neither of the
    // paths that made attribution load-bearing (burrow_share_into admits only
    // ANON + the weave DMA subtype; loom_resolve_buf admits only ANON), so this
    // region is settled by destroy or by exit and by nobody else -- but the
    // record costs one store and means no settler anywhere has to KNOW that.
    burrow_charge_record(b, p, npages);
    burrow_unref(b);
    spin_unlock(&p->as->lock);

    *out_writer = wva;
    *out_exec   = xva;
    return 0;

fail_unmap_writer:
    (void)burrow_unmap(p, wva, length);
    // burrow_unmap dropped the writer's mapping ref; the construction handle
    // below is then the last reference and frees the Burrow.
fail_unref:
    burrow_unref(b);
    proc_page_uncharge(p, npages);
    spin_unlock(&p->as->lock);
    return -T_E_NOMEM;
}

// SYS_JIT_CREATE: the mechanism above, plus the copy-out of the alias pair.
s64 sys_jit_create_for_proc(struct Proc *p, u64 length_raw, u64 out_va) {
    if (!p)                                          return -T_E_INVAL;

    // The out-buffer check comes AFTER the cap check inside the core would
    // run, so validate it there-and-back: check the cap first (so a capless
    // caller learns nothing from the buffer check), then the buffer, then act.
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_JIT) == 0)
        return -T_E_ACCES;
    if (!sys_validate_user_buf(out_va, sizeof(struct t_jit_region)))
        return -T_E_FAULT;

    u64 wva = 0, xva = 0;
    s64 rc = sys_jit_create_region(p, length_raw, &wva, &xva);
    if (rc != 0) return rc;

    // Copy out with NO lock held. uaccess can fault -> demand-page -> which
    // takes vma_lock, so doing this under the lock would self-deadlock. This is
    // the REVENANT R-5-F1 rule (no faulting uaccess under a lock the fault path
    // needs) obeyed at the point where it costs nothing.
    // uaccess_copy_out returns 0 on success / -1 on fault -- NOT a byte count
    // (arch/arm64/uaccess.h). Comparing against sizeof(reg) would treat every
    // successful copy as a fault.
    struct t_jit_region reg = { .writer_va = wva, .exec_va = xva };
    if (uaccess_copy_out(out_va, &reg, sizeof(reg)) != 0) {
        // The caller never learns where the region landed, so it could never
        // destroy it -- tear it down here rather than leak a region with no
        // reachable name. sys_jit_destroy_for_proc is the same teardown the
        // caller would have performed, so this cannot diverge from it.
        (void)sys_jit_destroy_for_proc(p, wva);
        return -T_E_FAULT;
    }
    return 0;
}

static s64 sys_jit_create_handler(u64 length_raw, u64 out_va) {
    struct Thread *t = current_thread();
    if (!t)                                          return -T_E_INVAL;
    return sys_jit_create_for_proc(t->proc, length_raw, out_va);
}

// SYS_JIT_DESTROY: tear down BOTH aliases of the region whose writer alias
// starts at writer_va, and free the backing pages.
//
// Identified by the WRITER VA alone rather than by a (writer, exec) pair: the
// kernel already knows the pairing (they share a Burrow), so taking one name
// makes it structurally impossible to destroy half a region or to pass two VAs
// belonging to different regions. Not CAP_JIT-gated -- see the syscall.h note;
// releasing memory you already own is not an exercise of the emit authority,
// and gating it would turn a capability expiry into a leak.
s64 sys_jit_destroy_for_proc(struct Proc *p, u64 writer_va) {
    if (!p)                                          return -T_E_INVAL;
    if (writer_va & (PAGE_SIZE - 1))                 return -T_E_INVAL;

    spin_lock(&p->as->lock);

    struct Vma *w = vma_lookup(p, writer_va);
    // Must be the BASE of the writer alias, not merely a VA inside it -- a
    // partial teardown has no meaning for a code region.
    if (!w || w->vaddr_start != writer_va || !jit_vma_is_writer(w)) {
        spin_unlock(&p->as->lock);
        return -T_E_INVAL;
    }

    struct Vma *x = jit_find_peer_locked(p, w);
    if (!x) {
        // A writer alias with no peer is a kernel invariant violation, not a
        // user error: SYS_JIT_CREATE installs both or neither, and nothing
        // else can remove one alias of a code region (SYS_BURROW_DETACH's
        // window check does not distinguish them, but it exact-matches
        // geometry and would take the whole VMA -- which is why destroy
        // refuses to guess rather than tearing down a lone half).
        spin_unlock(&p->as->lock);
        return -T_E_INVAL;
    }

    u64 length  = w->vaddr_end - w->vaddr_start;
    u64 exec_va = x->vaddr_start;
    // #106: recompute the create-time charge. `length` is the VMA span, which
    // IS the page-rounded length create passed to burrow_backing_pages, so the
    // refund reproduces the charge exactly.
    u32 npages  = (u32)burrow_backing_pages(length);

    // CL-7k-3 audit F3: validate the exec alias' geometry BEFORE touching
    // either mapping. Both burrow_unmaps below are issued unconditionally, so
    // if the exec unmap could fail while the writer unmap succeeded, the result
    // would be an orphaned RX alias with no writer -- the exact residue
    // jit.destroy_tears_down_both calls "must never survive", and one this
    // syscall then permanently refuses to clean up (no peer -> -EINVAL),
    // stranding its I-32 charge until Proc exit.
    //
    // Unreachable today: create installs both aliases at the same length. But
    // that is an UNASSERTED invariant of this function, and the neighbouring
    // unreachability claim just below (that nothing else can remove one alias)
    // was FALSE until this round's detach gate landed. Assert it rather than
    // inherit it.
    if (x->vaddr_end - x->vaddr_start != length) {
        spin_unlock(&p->as->lock);
        return -T_E_INVAL;
    }

    // Order is immaterial to correctness -- the #847 dual count frees the pages
    // only after the second unmap -- but tearing the EXEC alias down first
    // means that at no instant does an executable view of the region outlive
    // its writable partner, which keeps the "code is reachable only as a
    // complete region" reading true even mid-teardown.
    // #131/#132: claim the charge BEFORE the unmaps -- the record lives on the
    // Burrow, and a successful pair of unmaps frees it, so there is nothing to
    // read afterwards. Claiming is what makes the refund exactly-once; `npages`
    // above is kept only as the cross-check that the recomputation still agrees
    // with what was actually charged.
    // Snapshot the Burrow: both burrow_unmaps below free their Vma structs, so
    // `w` and `x` are dangling the moment the second one returns.
    struct Burrow *wb = w->burrow;
    u32 paid = burrow_charge_claim(wb, p);
    if (paid != 0 && paid != npages)
        extinction("SYS_JIT_DESTROY: charge record disagrees with the region's page count");

    int rc_x = burrow_unmap(p, exec_va, length);
    int rc_w = burrow_unmap(p, writer_va, length);
    if (rc_x == 0 && rc_w == 0) {
        if (paid) proc_page_uncharge(p, paid);
    } else if (paid) {
        // Neither alias was fully torn down, so the region -- and the charge
        // that belongs to it -- survives. Put the claim back for the retry or
        // for exit to settle. `wb` is still live: a partial teardown by
        // definition left a mapping holding it.
        burrow_charge_restore(wb, p, paid);
    }
    spin_unlock(&p->as->lock);

    return (rc_x == 0 && rc_w == 0) ? 0 : -T_E_INVAL;
}

static s64 sys_jit_destroy_handler(u64 writer_va) {
    struct Thread *t = current_thread();
    if (!t)                                          return -T_E_INVAL;
    return sys_jit_destroy_for_proc(t->proc, writer_va);
}

// SYS_ICACHE_SYNC: publish emitted bytes over [vaddr, vaddr+length).
//
// The cache maintenance runs on the KERNEL DIRECT-MAP alias of the backing
// pages, never on the user VA. That is a safety property, not an
// implementation detail: `dc cvau` / `ic ivau` are memory operations that can
// take translation faults, and a user VA is exactly the address a hostile or
// merely unlucky caller can arrange to be unmapped. The direct map is
// guaranteed present for all RAM, so no cache op here can fault.
//
// It is also architecturally exact. ARMv8 requires data caches to behave as
// PIPT, so cleaning ANY VA that maps the PA cleans the same line the user's
// write through the RW alias dirtied; and IC IVAU is specified to invalidate
// every alias of the PA. This is precisely how Linux's flush_icache_range
// publishes module text written through the linear map.
s64 sys_icache_sync_for_proc(struct Proc *p, u64 vaddr, u64 length) {
    if (!p)                                          return -T_E_INVAL;
    if (length == 0)                                 return -T_E_INVAL;
    if (vaddr + length < vaddr)                      return -T_E_INVAL;  // wrap
    if (length > JIT_REGION_MAX)                     return -T_E_INVAL;

    spin_lock(&p->as->lock);

    // The range must be contained in ONE alias of ONE live code region. Either
    // alias names the range legitimately (both map the same physical pages), so
    // a JIT may sync through whichever pointer it happens to hold.
    struct Vma *v = vma_lookup(p, vaddr);
    if (!v || !v->burrow || v->burrow->magic != VMO_MAGIC ||
        v->burrow->type != BURROW_TYPE_CODE ||
        vaddr + length > v->vaddr_end) {
        spin_unlock(&p->as->lock);
        return -T_E_INVAL;
    }

    struct Burrow *b = v->burrow;
    if (!b->pages) {
        spin_unlock(&p->as->lock);
        return -T_E_INVAL;
    }
    // Byte offset of the range within the Burrow, and a handle ref so the
    // pages survive a sibling thread's concurrent SYS_JIT_DESTROY while we
    // sync outside the lock.
    u64 off = (vaddr - v->vaddr_start) + v->burrow_offset;
    paddr_t base_pa = page_to_pa(b->pages);
    u64 bsize = (u64)b->size;
    burrow_ref(b);

    spin_unlock(&p->as->lock);

    // Defensive: the VMA span was validated against vaddr_end above, and
    // burrow_map installs a VMA no larger than its Burrow, so this cannot
    // trip -- but the arithmetic below indexes physical memory, so it is
    // checked rather than assumed.
    if (off + length > bsize) {
        burrow_unref(b);
        return -T_E_INVAL;
    }

    // Walk page by page: the region is physically contiguous (a CODE Burrow is
    // one alloc_pages chunk), but the direct map is addressed per page and
    // arch_icache_sync_range takes a kernel VA, so sync each page's span.
    // Bounded by JIT_REGION_MAX / PAGE_SIZE iterations.
    u64 done = 0;
    while (done < length) {
        u64 cur      = off + done;
        u64 page_off = cur & (PAGE_SIZE - 1);
        u64 chunk    = PAGE_SIZE - page_off;
        if (chunk > length - done) chunk = length - done;
        u8 *kva = (u8 *)pa_to_kva(base_pa + (cur & ~(u64)(PAGE_SIZE - 1)));
        arch_icache_sync_range(kva + page_off, (size_t)chunk);
        done += chunk;
    }

    // Dropped outside vma_lock. If a sibling destroyed the region while we
    // synced, this is the last reference and frees it here -- correct, and the
    // reason the ref was taken at all.
    burrow_unref(b);
    return 0;
}

static s64 sys_icache_sync_handler(u64 vaddr, u64 length) {
    struct Thread *t = current_thread();
    if (!t)                                          return -T_E_INVAL;
    return sys_icache_sync_for_proc(t->proc, vaddr, length);
}

// =============================================================================
// Weft -- the per-flow capability network dataplane EL0 delivery (Weft-6a-2;
// NET-THROUGHPUT.md section 6). kernel/weft.c owns the share_id registry + the
// binding lifecycle; these two syscalls wire a /net flow's shared ring into a
// guest. grant-is-the-share: holding the flow's data fd is the capability; the
// share_id is a kernel-internal join key (netd->kernel via Rweft, never to the
// guest -- the RDMA-rkey shape, section 4.6), so a guest cannot forge a mapping.
// =============================================================================

// SYS_WEFT_SHARE: register the caller's ANON ring Burrow (mapped whole at
// ring_va, RW / no-exec) as a per-flow ring + mint a share_id. The netd side.
s64 sys_weft_share_for_proc(struct Proc *p, u64 ring_va, u64 ring_size_raw) {
    if (!p)                                          return -1;
    // Weft-7 F1: gate the share to the NIC-owning driver tier (CAP_HW_CREATE).
    // SYS_WEFT_SHARE registers a per-flow ring + mints a share_id that ONLY netd's
    // Rweft ever hands the kernel to claim -- a non-driver caller's share_id is
    // never returned in any Rweft, so it is unclaimable and the call has NO
    // legitimate non-driver use. Ungated, any EL0 Proc could loop SYS_BURROW_ATTACH
    // + SYS_WEFT_SHARE to squat the fixed WEFT_MAX_SHARES registry (each entry held
    // until the squatter exits), starving the trusted netd's weft_ensure -> every
    // flow system-wide silently falls back to byte-copy (a cross-Proc availability
    // DoS on a shared global resource). CAP_HW_CREATE is the same driver-tier gate
    // SYS_MMIO/IRQ/DMA/PCI_CREATE use; netd holds it (the warden confers it), an
    // ordinary user Proc does not. ACQUIRE load: proc_become_legate writes caps
    // cross-thread (the A-4a clearance redeem).
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;
    if (ring_size_raw == 0)                          return -1;
    if (ring_size_raw > BURROW_ATTACH_MAX)           return -1;

    // The ring is whole-Burrow + page-granular (netd allocated it via
    // SYS_BURROW_ATTACH, which page-rounds), so round the request the same way
    // and require it to equal the backing Burrow's size below (the whole-ring
    // contract -- a partial share is rejected).
    u64 ring_size = (ring_size_raw + (PAGE_SIZE - 1)) & ~(u64)(PAGE_SIZE - 1);

    // Resolve ring_va -> the caller's ANON ring Burrow, validate, and take a
    // TEMPORARY ref so v survives the gap between dropping vma_lock and
    // weft_share_register taking its own (registration) ref -- a multi-thread
    // netd could SYS_BURROW_DETACH the ring concurrently in that window.
    spin_lock(&p->as->lock);
    struct Vma *vma = vma_lookup(p, ring_va);
    if (!vma || vma->burrow == NULL || vma->vaddr_start != ring_va) {
        spin_unlock(&p->as->lock);
        return -1;
    }
    struct Burrow *v = vma->burrow;
    // Admission (G-2 + Warp-2): ANON (a netd flow ring), or a KERNEL-MINTED
    // share-admissible DMA subtype -- the device-passive weave (tapestryd's
    // framebuffer backing; TAPESTRY.md §18.1) or the GPU BO (GPU-DESIGN.md
    // §6.1; the device-WRITTEN argument on KObj_DMA.gpu_bo). Plain DMA
    // (virtqueue / descriptor class) + MMIO fail closed -- the same
    // structural gate burrow_share_into enforces at claim time; the
    // register-side copy keeps an inadmissible region out of the registry
    // entirely, and the two MUST widen together (the Warp-2b test failed
    // exactly here when only the claim side was widened). RW (no exec -- the
    // share is RW-only, W^X like SYS_BURROW_ATTACH); whole-region
    // (ring_size == the Burrow size).
    bool share_admissible = (v->type == BURROW_TYPE_DMA &&
                             v->kobj_dma != NULL &&
                             (v->kobj_dma->weave || v->kobj_dma->gpu_bo)) ||
                            (v->type == BURROW_TYPE_HOSTMEM &&
                             v->kobj_pci != NULL);
    // V-2: BURROW_TYPE_HOSTMEM joins the register-side gate IN LOCKSTEP with the
    // claim side (burrow_share_into). The two MUST widen together -- a hostmem
    // Burrow admitted at claim but rejected here is the Warp-2b half-widen bug.
    if (v->type != BURROW_TYPE_ANON && !share_admissible) {
        spin_unlock(&p->as->lock);
        return -1;
    }
    if (vma->prot & VMA_PROT_EXEC) {
        spin_unlock(&p->as->lock);
        return -1;
    }
    if ((u64)burrow_get_size(v) != ring_size) {
        spin_unlock(&p->as->lock);
        return -1;
    }
    burrow_ref(v);                       // temp ref: keep v alive across the gap
    spin_unlock(&p->as->lock);

    u64 share_id = weft_share_register(p, v);   // takes its OWN registration pin
    burrow_unref(v);                            // drop the temp ref
    if (share_id == 0) return -1;               // full registry -> the flow stays byte-copy

    // share_id is a u64 monotonic from 1; it stays well below the s64 sign bit
    // (a positive return) until 2^63 zero-copy flows -- unreachable. Cast.
    return (s64)share_id;
}

// The claim-and-map half of SYS_WEFT_MAP (after Tweft returned a share_id).
// Claims the share (consume-once -> the registration pin transfers to us), maps
// the ring whole into the guest, records the binding. Returns the guest ring VA,
// dropping every ref it took on any failure.
static s64 weft_map_claimed(struct Proc *p, struct dev9p_priv *priv,
                            u64 share_id, u32 ring_entries, u64 hint_va) {
    (void)hint_va;   // v1.0: the kernel picks the VA (hint reserved for v1.x)

    struct Burrow *v = weft_share_claim(share_id);
    if (!v) {
        // The id was already consumed. If a concurrent SYS_WEFT_MAP on this SAME
        // data fd won the race (netd returned the same share_id idempotently for
        // the two Tweft(F)), return its cached VA; else it is a genuinely bad /
        // replayed / forged / UNREGISTERED id (SYS_WEFT_UNSHARE disarmed it --
        // the retire-vs-claim NoStaleMap gate: removal-before-free + this
        // live-registry lookup, tapestry_present.tla Map's wstate guard).
        struct weft_binding *winner = __atomic_load_n(&priv->weft, __ATOMIC_ACQUIRE);
        if (winner) return (s64)winner->guest_va;
        return -1;
    }

    // G-2 kind decision: the kernel-minted Burrow type is the authority; the
    // server's declared geometry must agree (ring: entries != 0; weave:
    // entries == 0), else fail closed dropping the claimed pin -- a mismatch
    // means the server's declaration contradicts its own registered region.
    int kind = weft_claimed_kind(v, ring_entries);
    if (kind < 0) {
        burrow_unref(v);
        return -1;
    }

    size_t bsize = burrow_get_size(v);

    // Place + share the whole region into the guest's burrow-attach window (so
    // the returned VA is detach-able by the native client, the SYS_BURROW_ATTACH
    // shape). burrow_share_into takes the guest mapping ref (mapping_count) AND
    // charges the guest's shared-in budget (Proc.shared_map_pages, the I-32
    // fifth axis -- R2-F3): the pages are the SHARER's commit, so page_count is
    // untouched, but the cross-Proc pin is bounded + accounted.
    spin_lock(&p->as->lock);
    u64 va;
    if (vma_find_gap(p, bsize, EXEC_USER_BURROW_BASE, EXEC_USER_BURROW_TOP, &va) != 0) {
        spin_unlock(&p->as->lock);
        burrow_unref(v);                 // drop the claimed registration pin
        return -1;
    }
    if (burrow_share_into(p, v, va, VMA_PROT_RW) != 0) {
        spin_unlock(&p->as->lock);
        burrow_unref(v);
        return -1;
    }
    spin_unlock(&p->as->lock);

    // Build the binding -- it will OWN the registration pin (transferred from
    // the claim). RING: compute the kernel-private ring view (geometry) from
    // the Burrow's KVA + the netd-reported ring_entries. WEAVE: no view (the
    // §18.11 F10 framebuffer branch -- the map IS the deliverable; the kind
    // gate keeps every Tweftio consumer off it); record the mapping pid for
    // the clunk-unmap. On OOM / invalid geometry / a non-weave Burrow, drop
    // BOTH the guest mapping AND the pin.
    struct weft_binding *b = weft_kind_maponly(kind)
        ? weft_binding_alloc_maponly(v, va, (u32)bsize, p->pid)
        : weft_binding_alloc(v, va, (u32)bsize, ring_entries);
    if (!b) {
        spin_lock(&p->as->lock);
        (void)burrow_unmap(p, va, bsize);
        spin_unlock(&p->as->lock);
        burrow_unref(v);
        return -1;
    }

    // Install atomically. A concurrent SYS_WEFT_MAP on this SAME fd (a
    // multi-thread guest) that ALSO built a binding (its own ring, a DISTINCT
    // share_id) races us: exactly one CAS wins. The loser tears its ring down
    // (guest mapping + the pin via weft_binding_release) and returns the
    // winner's cached VA -- no leak, no double-owned priv->weft.
    struct weft_binding *expected = NULL;
    if (!__atomic_compare_exchange_n(&priv->weft, &expected, b, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        spin_lock(&p->as->lock);
        (void)burrow_unmap(p, va, bsize);
        spin_unlock(&p->as->lock);
        weft_binding_release(b);         // unrefs b->burrow (the pin) + frees b
        return (s64)expected->guest_va;  // expected == the winner's live binding
    }
    // G-3 (R2-F3): a WEAVE binding enters the orphan reaper's registry so a
    // dead compositor's stale client mapping is force-reclaimed after the
    // grace. Registered only by the CAS WINNER (the loser's binding was just
    // torn down), with no locks held, while the caller's #844 Spoor ref
    // still pins priv (a sibling-thread close cannot run until the syscall's
    // handle_put) -- so the register-vs-close order is structural. The
    // session pointers are borrowed via priv (see weft.h).
    if (weft_kind_maponly(kind))
        weft_reap_register(b, priv->attached_owner, priv->client);
    return (s64)va;
}

// SYS_WEFT_MAP: lazily map a /net data fd's per-flow ring into the caller. The
// guest side.
s64 sys_weft_map_for_proc(struct Proc *p, hidx_t data_fd, u64 hint_va) {
    if (!p) return -1;

    struct Handle dh;
    if (handle_get(p, data_fd, &dh) != 0)  return -1;
    if (dh.kind != KOBJ_SPOOR)             { handle_put(&dh); return -1; }
    struct Spoor *spoor = (struct Spoor *)dh.obj;

    struct dev9p_priv *priv = dev9p_priv_of(spoor);
    if (!priv) { handle_put(&dh); return -1; }   // not a dev9p file

    // Idempotent fast path: already mapped -> return the cached VA (no Tweft).
    struct weft_binding *existing = __atomic_load_n(&priv->weft, __ATOMIC_ACQUIRE);
    if (existing) {
        u64 va = existing->guest_va;
        handle_put(&dh);
        return (s64)va;
    }

    // Resolve (client, fid F) + issue Tweft(F) -> Rweft(share_id) on the shared
    // /net client. The blocking round-trip is the #841 elected reader +
    // #811-death-interruptible; the handle_get ref keeps the Spoor (hence the
    // client -- dev9p's lifecycle invariant) alive across it.
    struct p9_client *client = NULL;
    u32 fid = 0;
    if (dev9p_client_fid(spoor, &client, &fid) != 0) { handle_put(&dh); return -1; }

    struct p9_weft_geom geom;
    int e = p9_client_weft(client, fid, &geom);
    if (e != 0) { handle_put(&dh); return -1; }   // e.g. a server with no Tweft handler (pre-6b)

    s64 r = weft_map_claimed(p, priv, geom.share_id, geom.ring_entries, hint_va);
    handle_put(&dh);
    return r;
}

// SYS_WEFT_UNSHARE: disarm one of the caller's own un-claimed shares (G-2 --
// the retire/reweave NoStaleMap disarm + the #289 minted-but-unclaimed GC).
s64 sys_weft_unshare_for_proc(struct Proc *p, u64 share_id) {
    if (!p) return -1;
    // The same driver-tier gate as SYS_WEFT_SHARE (Weft-7 F1); the owner check
    // inside weft_share_unregister is the real authority -- only the
    // registrant's own entry is removable.
    if ((__atomic_load_n(&p->caps, __ATOMIC_ACQUIRE) & CAP_HW_CREATE) == 0)
        return -1;
    return (s64)weft_share_unregister(p, share_id);
}

static s64 sys_weft_share_handler(u64 ring_va, u64 ring_size_raw) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                              return -1;
    return sys_weft_share_for_proc(t->proc, ring_va, ring_size_raw);
}

static s64 sys_weft_unshare_handler(u64 share_id) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                              return -1;
    return sys_weft_unshare_for_proc(t->proc, share_id);
}

static s64 sys_weft_map_handler(u64 data_fd_raw, u64 hint_va) {
    struct Thread *t = current_thread();
    if (!t || !t->proc)                              return -1;
    return sys_weft_map_for_proc(t->proc, (hidx_t)data_fd_raw, hint_va);
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
    // #106: the buddy-rounded occupancy. loom_create's ring Burrow is an
    // ordinary burrow_create_anon, and ring_size is page-rounded but NOT
    // power-of-two rounded (it is the sum of four 64-aligned regions), so a
    // 3-page ring occupies 4. The ring is detached through the ordinary
    // SYS_BURROW_DETACH path, whose ANON arm recomputes burrow_backing_pages
    // from the exact-matched VMA length -- the same ring_size -- so charge and
    // refund agree.
    u32 ring_pages = (u32)burrow_backing_pages((size_t)l->ring_size);

    // Map the ring RW into the burrow-attach window. burrow_map takes its OWN
    // mapping_count ref (vma_alloc -> burrow_acquire_mapping); the Loom keeps
    // its handle_count ref, so the ring stays alive while EITHER side holds it
    // (the #847 dual-refcount). vma_find_gap + burrow_map under one vma_lock so
    // a sibling thread cannot claim the same gap.
    spin_lock(&p->as->lock);
    u64 vaddr;
    if (vma_find_gap(p, (size_t)l->ring_size, EXEC_USER_BURROW_BASE,
                     EXEC_USER_BURROW_TOP, &vaddr) != 0) {
        spin_unlock(&p->as->lock);
        loom_unref(l);
        return -1;
    }
    if (!proc_page_charge(p, ring_pages)) {
        spin_unlock(&p->as->lock);
        loom_unref(l);
        return -1;                                   // over the per-Proc page cap
    }
    if (burrow_map(p, l->ring, vaddr, (size_t)l->ring_size, VMA_PROT_RW) != 0) {
        proc_page_uncharge(p, ring_pages);
        spin_unlock(&p->as->lock);
        loom_unref(l);
        return -1;
    }
    spin_unlock(&p->as->lock);

    // Loom-4c: spawn the SQPOLL poll-thread BEFORE installing the handle, so
    // every failure path below reclaims it through loom_unref -> loom_free's
    // join (loom_free sees l->sqpoll set and stops + reaps the kthread). The
    // earlier failure paths (above) ran with l->sqpoll == NULL, so loom_free
    // skipped the join there. The kthread immediately parks (no SQEs yet).
    if (flags & LOOM_SETUP_SQPOLL) {
        // The F1 thread-budget charge FIRST: the kthread counts against
        // this Proc's PROC_THREAD_MAX (its worker, whoever runs it). A
        // refusal here is the I-32 floor working, same as the page charge
        // above. loom_unref -> loom_free settles the charge via
        // sqpoll_charged on every later path, including a start failure.
        if (!proc_sqpoll_charge(p)) {
            spin_lock(&p->as->lock);
            (void)burrow_unmap(p, vaddr, (size_t)l->ring_size);
            proc_page_uncharge(p, ring_pages);
            spin_unlock(&p->as->lock);
            loom_unref(l);
            return -1;
        }
        l->sqpoll_owner = p;
        l->sqpoll_charged = true;
        if (loom_start_sqpoll(l) != 0) {
            spin_lock(&p->as->lock);
            (void)burrow_unmap(p, vaddr, (size_t)l->ring_size);
            proc_page_uncharge(p, ring_pages);       // #65: the ring VMA freed
            spin_unlock(&p->as->lock);
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
        spin_lock(&p->as->lock);
        (void)burrow_unmap(p, vaddr, (size_t)l->ring_size);
        proc_page_uncharge(p, ring_pages);           // #65: the ring VMA freed
        spin_unlock(&p->as->lock);
        loom_unref(l);
        return -1;
    }

    // #130: bind the I-32 charge ledger. Deliberately LAST -- after the final
    // failure path -- so it marks a fully-constructed Loom. Every rollback
    // above uncharges explicitly and then loom_unref's a Loom whose owner is
    // still NULL, so loom_free's uncharge cannot double-refund them. From here
    // on the Loom owns the settlement: whichever of {the ring VA's detach,
    // loom_free} frees the pages does the single uncharge.
    l->owner      = p;
    l->owner_pid  = p->pid;
    l->ring_pages = ring_pages;
    // #131/#132: and stamp the payer on the ring itself, so loom_free settles
    // by CLAIM rather than by trusting ring_pages. That makes the ring's refund
    // exactly-once against the ring VA's detach (which claims the same record)
    // instead of resting on the argument that the two can never both free it.
    burrow_charge_record(l->ring, p, ring_pages);

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
            spin_lock(&p->as->lock);
            (void)burrow_unmap(p, kp.ring_va, (size_t)kp.ring_size);
            spin_unlock(&p->as->lock);
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
// 0x0008 per territory.h; MNOEXEC (#217) is 0x0010. Mask out everything
// else — userspace supplying junk bits is rejected at the syscall layer
// (mount() in territory.c is silent on extra bits, but we want a tight
// contract at the boundary).
//
// Adding a flag REQUIRES adding it here: this allowlist is why a new bit is
// safe to introduce (an old caller's junk still fails) and equally why a new
// bit that is not listed is silently unusable -- the mount would just fail
// -1 with nothing naming the cause.
#define SYS_MOUNT_VALID_FLAGS  ((u32)(MREPL | MBEFORE | MAFTER | MCREATE | MNOEXEC))

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
    // RW-3 R2-F1: reject len > SYS_RW_STACK -- do NOT silently cap. For a
    // secret-scrub primitive, capping + returning success would silently
    // retain the tail of the buffer; the libthyla-rs wrapper documents -1 on
    // oversize, and SYS_PUTS/SYS_READDIR reject oversize the same way.
    // (CF-3 A kept this arm at the historical 4 KiB bound when SYS_RW_MAX
    // lifted to 128 KiB -- widening a REJECT bound is an ABI change this
    // chunk does not need.)
    if (len > SYS_RW_STACK)                           return -1;
    if (len == 0)                                    return 0;

    for (u64 i = 0; i < len; i++) {
        if (uaccess_store_u8(buf_va + i, 0) != 0)    return -1;
    }
    return 0;
}

// SYS_GETRANDOM — read kernel CSPRNG bytes into a user-VA buffer.
// CAP_CSPRNG_READ required. Caller's user-VA buffer is filled via
// 4 KiB kernel-stack scratch + uaccess_store_u8 per byte (the pre-CF-3
// SYS_READ bounce shape, kept here: the F237 partial-fault scrub wants
// the per-byte loop, and 4 KiB of entropy per call needs no bulk path).
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
    if (len > SYS_RW_STACK) len = SYS_RW_STACK;
    if (len == 0)                                    return 0;
    if (!kern_random_seeded())                       return -1;

    u8 scratch[SYS_RW_STACK];
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
// Lifetime of the executable Spoor (REVENANT R-4): PINNED by the SYS_SPAWN
// handler (exec_resolve_from_namespace transfers the ref) and clunked by the
// child's spawn_thunk after exec_setup_from_spoor reads the header + maps the
// segments. The args struct is also kmalloc'd + freed (lives across the rfork
// boundary, so it can't be on the caller's kernel stack — the caller may return
// to userspace before the child's thunk runs).

// The one place both spellings of the argv bounds are visible at once. exec.c
// cannot include syscall.h (the include cycle), so it carries its own names;
// this is where they are PROVEN equal rather than asserted to be by a comment.
// #178's whole complaint is mirrors nothing checks -- these two now are.
_Static_assert(EXEC_ARGV_MAX == SYS_SPAWN_ARGV_MAX,
               "exec.h argv-count mirror drifted from the syscall ABI");
_Static_assert(EXEC_ARGV_DATA_MAX == SYS_SPAWN_ARGV_DATA_MAX,
               "exec.h argv-bytes mirror drifted from the syscall ABI");

struct spawn_args {
    struct Spoor *exe;      // REVENANT R-4: the pinned executable; thunk clunks it
    size_t        exe_size; // stat'd file size (bounds the ELF segment-extent check)
};

__attribute__((noreturn))
static void sys_spawn_thunk(void *arg) {
    struct spawn_args *sa = (struct spawn_args *)arg;
    struct Spoor *exe = sa->exe;
    size_t exe_size   = sa->exe_size;
    kfree(sa);

    struct Thread *t = current_thread();
    if (!t) extinction("sys_spawn_thunk: no current_thread");
    struct Proc *p = t->proc;
    if (!p) extinction("sys_spawn_thunk: no proc");

    u64 entry = 0, sp = 0;
    // #359/#360: this thunk runs IRQ-ENABLED on a fresh thread (preemptible,
    // like a kthread). Its shared-lock holds (the REVENANT eager read + the
    // spoor_clunk on the dev9p pool client's c->lock) are safe because a plain
    // spin_lock hold now disables preemption per-THREAD (spinlock.h #360) --
    // the general rule that replaced the interim whole-thunk IRQ mask.
    int rc = exec_setup_from_spoor(p, exe, exe_size,
                                   /*prog_name=*/NULL, 0,   // D-4: native-only entry
                                   NULL, 0, 0, &entry, &sp);
    spoor_clunk(exe);
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

// exec_resolve_from_namespace (#58 / REVENANT R-4) -- resolve the program `name`
// in the CALLER's namespace + PIN the resulting executable Spoor, replacing the
// flat boot-cpio `devramfs_lookup`. Realizes I-28 + I-1 for the exec path: a
// binary in a mounted FS (a container root, the disk-backed Stratum FS) is
// executable, and a confined Proc can exec ONLY what its namespace names (the
// reverse-visibility leak closes -- a `stalk` miss is NULL, never a flat
// fallback). Resolution mirrors SYS_OPEN exactly: an absolute path from the
// Territory `root_spoor`, a relative name via the LS-4 cwd-join; OEXEC gates a
// per-component X-search on every directory hop and PERM_R|PERM_X on the final
// file (RW-3 R3-F1), and the A-3 OEXEC->RIGHT_READ open yields a readable Spoor.
//
// REVENANT R-4 retires the whole-binary slurp: the Spoor is PINNED (the ref is
// transferred to the caller) and the bytes are read LATER -- the header+phdrs
// + data segments by exec_setup_from_spoor in the CHILD's context, the text
// pages demand-paged by the R-2 fault arm -- so a binary of any size execs (the
// old SYS_SPAWN_BLOB_MAX 1-MiB cap is gone; only EXEC_FILE_MAX sanity-bounds the
// stat'd size). Runs in the parent's context (its Territory), like Unix exec.
// Returns the pinned Spoor (caller spoor_clunks) + *size_out, or NULL on any
// failure. Exported for the kernel-internal #58 tests.
struct Spoor *exec_resolve_from_namespace(struct Proc *p, const char *name,
                                          size_t name_len, size_t *size_out) {
    if (!p || !name || !size_out)                  return NULL;
    *size_out = 0;
    if (name_len == 0 || name_len > SYS_OPEN_PATH_MAX) return NULL;

    // start = the Territory root (atomic ref under ns_lock, RW-4 SA-F1). An
    // absolute name resolves from it directly; a relative name is cwd-joined
    // (territory_join_cwd) into an absolute path first -- exactly SYS_OPEN.
    struct Spoor *start = territory_root_ref(p->territory);
    if (!start)                                    return NULL;

    char joined[SYS_OPEN_PATH_MAX + 1];
    const char *rpath = name;
    u64 rlen = (u64)name_len;
    if (name[0] != '/') {
        int jl = territory_join_cwd(p->territory, name, (u64)name_len,
                                    joined, sizeof(joined));
        if (jl < 0)                                { spoor_clunk(start); return NULL; }
        rpath = joined;
        rlen  = (u64)jl;
    }

    struct Spoor *quarry = stalk(p, start, rpath, rlen, STALK_OPEN, 3u /* OEXEC */);
    spoor_clunk(start);   // borrowed by stalk; release the ref we took
    if (!quarry)                                   return NULL;
    if (!quarry->dev || !quarry->dev->read)        { spoor_clunk(quarry); return NULL; }

    // #217: MNOEXEC refuses exec as well as the R+X map, because a mount that
    // still permits exec is not noexec -- and a half-enforced flag is worse
    // than none, since it reads as a guarantee it does not make. Tested on the
    // RESOLVED quarry, i.e. after every mount cross, so the verdict follows the
    // device instance the bytes actually live on rather than the path spelling
    // used to reach them.
    if (!exec_map_vouched(p, quarry))              { spoor_clunk(quarry); return NULL; }

    // Size from stat -- bounds exec_setup_from_spoor's ELF segment-extent
    // validation + an EXEC_FILE_MAX sanity ceiling (the binary is NOT read here
    // anymore; only its identity is pinned). A truncated file surfaces later as
    // a short read in exec_setup_from_spoor -> clean exec failure, no partial map.
    struct t_stat st;
    if (spoor_stat_native(quarry, &st) != 0)       { spoor_clunk(quarry); return NULL; }
    if (st.size == 0 || (u64)st.size > EXEC_FILE_MAX) { spoor_clunk(quarry); return NULL; }

    *size_out = (size_t)st.size;
    return quarry;        // ref transferred to the caller (the spawn thunk clunks it)
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

    // #58 / REVENANT R-4: resolve + PIN the executable from the caller's
    // namespace (was the flat devramfs_lookup + whole-binary slurp). The bytes
    // are read later (header in the child, text demand-paged) -- no size cap.
    size_t exe_size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(p, name, name_len, &exe_size);
    if (!exe)                                          return -1;

    struct spawn_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        spoor_clunk(exe);
        return -1;
    }
    sa->exe      = exe;
    sa->exe_size = exe_size;

    int pid = rfork(RFPROC, sys_spawn_thunk, sa);
    if (pid < 0) {
        kfree(sa);
        spoor_clunk(exe);
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
    struct Spoor  *exe;        // REVENANT R-4: pinned executable; thunk clunks it
    size_t         exe_size;
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
    struct Spoor *exe  = sa->exe;
    size_t  exe_size   = sa->exe_size;
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
            spoor_clunk(exe);
            exits("fail-fd-install");
        }
        installed++;
    }
    (void)installed;

    u64 entry = 0, sp = 0;
    // #359/#360: preemptible fresh-thread exec; the c->lock holds are covered
    // by the spinlock preempt count (spinlock.h). See sys_spawn_thunk.
    int rc = exec_setup_from_spoor(p, exe, exe_size,
                                   /*prog_name=*/NULL, 0,   // D-4: native-only entry
                                   NULL, 0, 0, &entry, &sp);
    spoor_clunk(exe);
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

    // #58 / REVENANT R-4: resolve + PIN the executable (was the whole-binary slurp).
    size_t exe_size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(p, name, name_len, &exe_size);
    if (!exe) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

    struct spawn_with_fds_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        spoor_clunk(exe);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    sa->exe      = exe;
    sa->exe_size = exe_size;
    sa->fd_count  = fd_count;
    for (u32 i = 0; i < fd_count; i++) {
        sa->spoors[i] = bumped[i];
        sa->rights[i] = bumped_rights[i];
    }

    int pid = rfork(RFPROC, sys_spawn_with_fds_thunk, sa);
    if (pid < 0) {
        kfree(sa);
        spoor_clunk(exe);
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

    // #58 / REVENANT R-4: resolve + PIN the executable (was the whole-binary slurp).
    size_t exe_size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(p, name, name_len, &exe_size);
    if (!exe)                                          return -1;

    struct spawn_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        spoor_clunk(exe);
        return -1;
    }
    sa->exe      = exe;
    sa->exe_size = exe_size;

    int pid = rfork_with_caps(RFPROC, sys_spawn_thunk, sa, cap_mask);
    if (pid < 0) {
        kfree(sa);
        spoor_clunk(exe);
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

    // #58 / REVENANT R-4: resolve + PIN the executable (was the whole-binary slurp).
    size_t exe_size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(p, name, name_len, &exe_size);
    if (!exe) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

    struct spawn_with_fds_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        spoor_clunk(exe);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    sa->exe        = exe;
    sa->exe_size   = exe_size;
    sa->fd_count   = fd_count;
    sa->perm_flags = perm_flags;
    for (u32 i = 0; i < fd_count; i++) {
        sa->spoors[i] = bumped[i];
        sa->rights[i] = bumped_rights[i];
    }

    int pid = rfork_with_caps(RFPROC, sys_spawn_with_fds_thunk, sa, cap_mask);
    if (pid < 0) {
        kfree(sa);
        spoor_clunk(exe);
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
    // G-4: CONSOLE_RENDERER is gated NARROW (console-attach-only, the
    // CONSOLE_TRUSTED shape -- the pair reads all console output + injects
    // input, so only the boot trust anchor designates it) AND single-holder
    // (refused while a live renderer holds the role; the residual
    // two-concurrent-grants race is closed by proc_set_console_renderer's
    // claim-under-lock in the child thunk).
    if (perm_flags & SPAWN_PERM_CONSOLE_RENDERER) {
        if (!proc_is_console_attached(p))                          return -1;
        if (proc_test_console_renderer() != NULL)                  return -1;
    }
    // CL-5: MAY_RAISE_PAGE_BUDGET takes the MAY_POST_SERVICE one-hop shape
    // (console-attached OR an existing holder), so joey -> login -> shell ->
    // build-driver can carry it without any of them being console-attached.
    // Note this gates conferring the AUTHORITY; the raise it authorizes is
    // still bounded by PROC_PAGE_HARD_MAX in proc_spawn_budget_resolve.
    if ((perm_flags & SPAWN_PERM_MAY_RAISE_PAGE_BUDGET)
            && !proc_is_console_attached(p)
            && !proc_may_raise_page_budget(p))                     return -1;
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
    if (perm_flags & SPAWN_PERM_CONSOLE_RENDERER) {
        // G-4: single-holder CAS-under-lock. A -1 here means a concurrent
        // grant won the race (both passed the parent-side check before either
        // child stamped); the loser proceeds WITHOUT the flag and the
        // drain/feed open gate refuses it -- fail-closed, never an extinction
        // (the child is otherwise a valid Proc).
        (void)proc_set_console_renderer(p);
    }
    if (perm_flags & SPAWN_PERM_MAY_RAISE_PAGE_BUDGET) {
        proc_mark_may_raise_page_budget(p);   // CL-5: the raise authority
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
    struct Spoor  *exe;        // REVENANT R-4: pinned executable; thunk clunks it
    size_t         exe_size;
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
    // CL-5: the RESOLVED anon page budget. The parent computed it (it needs the
    // parent's own budget + raise authority, neither reachable from the child),
    // so the thunk only stamps. Always non-zero: proc_spawn_budget_resolve
    // returns the inherited budget when the caller asked for none, and a 0 from
    // it means REFUSE, which fails the spawn before we ever get here.
    u32            page_budget;
    // VIVARIUM V-1b: stamp the child PHENO_LINUX in the thunk (section 12.1
    // rule 1 -- the vivarium's declaration). false -> the child keeps the
    // phenotype rfork inherited (a native parent's child stays native).
    bool pheno_linux;
    // DISTRO D-4: the name the caller spawned, carried into the child because
    // the PT_INTERP rewrite has to put it on the interpreter's command line and
    // the resolution that consumed it happened in the PARENT. Inline rather
    // than a second heap block: it is bounded by SYS_SPAWN_NAME_MAX, and a
    // pointer here would add a free path to five error interleavings that
    // currently have exactly one heap object to worry about.
    //
    // NOT reconstructed from exe->path on the child side: I-33 makes the
    // Spoor's Path cosmetic, so an exec whose success turned on it would be the
    // invariant's own counterexample.
    u32            name_len;
    char           name[SYS_SPAWN_NAME_MAX + 1];
};

__attribute__((noreturn))
static void sys_spawn_full_argv_thunk(void *arg) {
    struct spawn_full_argv_args *sa = (struct spawn_full_argv_args *)arg;
    struct Spoor *exe     = sa->exe;
    size_t  exe_size      = sa->exe_size;
    u32     fd_count      = sa->fd_count;
    u32     perm_flags    = sa->perm_flags;
    char   *argv_data     = sa->argv_data;
    u32     argv_data_len = sa->argv_data_len;
    u32     argc          = sa->argc;
    struct spawn_identity identity = sa->identity;   // A-1a: copy before kfree
    u32     page_budget   = sa->page_budget;         // CL-5: copy before kfree
    struct spawn_allowance allowance;                // step 5: copy before kfree
    spawn_allowance_copy(&allowance, &sa->allowance);
    bool    pheno_linux   = sa->pheno_linux;         // V-1b: copy before kfree
    u32     name_len      = sa->name_len;            // D-4: copy before kfree
    if (name_len > SYS_SPAWN_NAME_MAX) name_len = SYS_SPAWN_NAME_MAX;
    char    name[SYS_SPAWN_NAME_MAX + 1];
    for (u32 i = 0; i < name_len; i++) name[i] = sa->name[i];
    name[name_len] = '\0';
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

    // CL-5: stamp the parent-resolved anon budget before the child can charge
    // anything. Between rfork and here the child has run only apply_spawn_perms
    // (flag RMWs; no charge, no mapping), so nothing observes the inherited
    // value except a /proc reader.
    //
    // NOTE the ordering argument, precisely: exec is NOT the first charger.
    // kernel/exec.c calls proc_page_charge ZERO times -- exec's segments and
    // stack are eager burrow_create_anon, which the #65 posture deliberately
    // leaves uncharged ("exec-image one-shot bounded"). The only four chargers
    // are SYS_BURROW_ATTACH, SYS_JIT_CREATE, the Loom ring, and the lazy-anon
    // fault arm, all of which are post-userland_enter. So the stamp is safe with
    // MORE margin than "before exec" claims -- and stamping before exec anyway
    // means a FUTURE exec-time charge (the recorded REVENANT per-page I-32 seam)
    // is bounded automatically rather than needing this ordering revisited.
    // (The count was "three" until CL-7k added the JIT; keep it current -- this
    // is the census #106 had to redo from scratch to find every charge site.)
    //
    // Corollary worth not misreading: a REDUCED budget does not today bound a
    // child's exec-image anon. That gap is the seam's, not this mechanism's.
    // Atomic store -- devproc reads this lockless cross-Proc (the page_count /
    // page_peak / vma_count discipline).
    __atomic_store_n(&p->page_budget, page_budget, __ATOMIC_RELEASE);
    // I-32 (A): the AUTHORIZATION above is not the cap anything enforces --
    // addrspace_charge_pages reads the ADDRESS SPACE's. rfork seeded that space
    // from the PARENT's budget, which is right for the inherit case and wrong
    // for the whole point of CL-5: a caller that passes an explicit budget gets
    // a RESOLVED value (proc_spawn_budget_resolve) that may be a reduction or an
    // authorized raise, and without this store the child would run under the
    // parent's cap while /proc reported the resolved one.
    //
    // Guarded on SOLE OWNERSHIP rather than on "every spawn is RFPROC today":
    // that is true of all five call sites, but it is a property a later commit
    // could quietly void, and the failure would be silent and severe -- stamping
    // a SHARED space rewrites the cap out from under the Proc that owns it. The
    // refcount asks the question the safety actually depends on. A shared space
    // keeps its own cap (the RFMEM argument in addrspace.h); the authorization
    // stamped above still records what this Proc was granted.
    //
    // Safe here for the same reason the store above is: pre-EL0, no peer thread
    // yet, and ahead of all four chargers enumerated above -- so no charge has
    // been decided against the value being replaced.
    if (p->as && __atomic_load_n(&p->as->ref, __ATOMIC_ACQUIRE) == 1)
        __atomic_store_n(&p->as->page_budget, page_budget, __ATOMIC_RELEASE);
    // VIVARIUM V-1b: stamp the declared phenotype BEFORE exec_setup and
    // before EL0 -- the child has no peer thread yet, so a plain store is
    // race-free (the identity/allowance set-once-before-EL0 contract), and
    // exec can already read it for the section 12.1 rule-4 mismatch
    // diagnostic. Descendants inherit it via rfork (rule 2). No gate: a
    // phenotype confers ABI shape, never authority (I-43).
    if (pheno_linux) p->phenotype = PHENO_LINUX;

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
    // (mirror the fd-install failure path: clunk the still-pinned exe + free argv).
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
            spoor_clunk(exe);
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
            spoor_clunk(exe);
            kfree(argv_data);
            exits("fail-fd-install");
        }
        installed++;
    }
    (void)installed;

    u64 entry = 0, sp = 0;
    // #359/#360: preemptible fresh-thread exec; the c->lock holds are covered
    // by the spinlock preempt count (spinlock.h). See sys_spawn_thunk.
    int rc = exec_setup_from_spoor(p, exe, exe_size,
                                   name, name_len,
                                   argv_data, argv_data_len, argc,
                                   &entry, &sp);
    spoor_clunk(exe);
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
        caps_t cap_mask, u32 perm_flags,
        const u32 *fds, u32 fd_count,
        u32 eff_budget,
        const struct spawn_identity *id,
        const struct spawn_allowance *want_allowance,
        u32 pheno_flags) {
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
    if (pheno_flags & ~SPAWN_PHENO_FLAGS_ALL)           return -1;

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

    // #58 / REVENANT R-4: resolve + PIN the executable (was the whole-binary slurp).
    size_t exe_size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(p, name, name_len, &exe_size);
    if (!exe) {
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }

    // Kernel-side argv copy. Lifetime: owned by the spawn_args struct
    // until the thunk's exec_setup_from_spoor consumes it. Free-on-error
    // paths handle every interleaving below.
    char *argv_data_copy = NULL;
    if (argv_data_len > 0) {
        argv_data_copy = kmalloc(argv_data_len, 0);
        if (!argv_data_copy) {
            spoor_clunk(exe);
            for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
            return -1;
        }
        for (u32 i = 0; i < argv_data_len; i++) argv_data_copy[i] = argv_data[i];
    }

    struct spawn_full_argv_args *sa = kmalloc(sizeof(*sa), KP_ZERO);
    if (!sa) {
        if (argv_data_copy) kfree(argv_data_copy);
        spoor_clunk(exe);
        for (u32 j = 0; j < fd_count; j++) spoor_unref(bumped[j]);
        return -1;
    }
    sa->exe           = exe;
    sa->exe_size      = exe_size;
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
    sa->page_budget   = eff_budget;   // CL-5: parent-resolved; the thunk stamps it
    // V-1b: carry the phenotype declaration (0 -> KP_ZERO left it false ->
    // the child inherits via rfork).
    sa->pheno_linux = (pheno_flags & SPAWN_PHENO_LINUX) != 0;
    // D-4: carry the caller's own name for the program. Bounded + NUL-checked
    // by this function's entry gate above, so the copy is a straight one.
    sa->name_len = (u32)name_len;
    for (size_t i = 0; i < name_len; i++) sa->name[i] = name[i];
    sa->name[name_len] = '\0';
    for (u32 i = 0; i < fd_count; i++) {
        sa->spoors[i] = bumped[i];
        sa->rights[i] = bumped_rights[i];
    }

    int pid = rfork_with_caps(RFPROC, sys_spawn_full_argv_thunk, sa, cap_mask);
    if (pid < 0) {
        kfree(sa);
        if (argv_data_copy) kfree(argv_data_copy);
        spoor_clunk(exe);
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
int sys_spawn_full_argv_budget_for_proc(struct Proc *p,
        const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        bool set_identity, u32 principal_id, u32 primary_gid,
        const u32 *supp_gids, u32 supp_gid_count,
        const struct spawn_allowance *want_allowance,
        u32 req_budget, u32 pheno_flags) {
    if (!p)                                             return -1;
    if (spawn_perm_grant_check(p, perm_flags) != 0)     return -1;
    // V-1b: unknown pheno bits reject (forward-compat); the known bit needs
    // NO grant gate -- a phenotype confers ABI shape, never authority (I-43),
    // so any Proc may declare its child's decode mode. See SPAWN_PHENO_LINUX.
    if (pheno_flags & ~SPAWN_PHENO_FLAGS_ALL)           return -1;

    // CL-5: resolve the requested anon budget against the caller's own budget +
    // raise authority. 0 back means REFUSE (over the hard cap, or a raise
    // without SPAWN_PERM_MAY_RAISE_PAGE_BUDGET) -- fail the spawn rather than
    // silently clamping, so a misconfigured build fails loudly instead of
    // dying later with an opaque OOM. Resolved in the PARENT's context because
    // only the parent's budget + flags decide it.
    u32 eff_budget = proc_spawn_budget_resolve(p, req_budget);
    if (eff_budget == 0)                                return -1;

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
                                                   argc, cap_mask, perm_flags,
                                                   fds, fd_count, eff_budget,
                                                   eff_id, want_allowance,
                                                   pheno_flags);
}

// Back-compat entry: no budget request and no phenotype declaration (0 == 
// inherit, for both). Keeps the pre-CL-5 / pre-V-1b signature for the
// existing callers + the kernel test suite.
int sys_spawn_full_argv_identity_for_proc(struct Proc *p,
        const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        bool set_identity, u32 principal_id, u32 primary_gid,
        const u32 *supp_gids, u32 supp_gid_count,
        const struct spawn_allowance *want_allowance) {
    return sys_spawn_full_argv_budget_for_proc(p, name, name_len, argv_data,
                                               argv_data_len, argc, fds,
                                               fd_count, cap_mask, perm_flags,
                                               set_identity, principal_id,
                                               primary_gid, supp_gids,
                                               supp_gid_count, want_allowance,
                                               /*req_budget=*/0u,
                                               /*pheno_flags=*/0u);
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
    // compat, same rationale as _pad_envp); a SET request requires a non-NULL
    // descriptor VA (the handler then copies + count-bounds it). The mmio/irq
    // count bounds + the narrowing gate are checked after the copy-in / in the
    // identity entry (so an over-count or a too-wide ask is a clean -1, never
    // a buffer overrun).
    if (req->allowance_flags & ~(u32)SPAWN_ALLOWANCE_FLAGS_ALL) return -1;
    // CL-5: page_budget claims the former _pad_allow slot at 92, so the old
    // "must be 0" reject is REPLACED by a range check. 0 still means inherit.
    // The authority decision (raise vs reduce) is NOT made here -- it needs the
    // caller's Proc, so it lives in proc_spawn_budget_resolve at the entry.
    if (req->page_budget > PROC_PAGE_HARD_MAX)         return -1;
    // VIVARIUM V-1b: pheno_flags. Both this and page_budget were authored
    // against _pad_allow on separate branches -- the collision the aux-2 merge
    // had to arbitrate, and the reason the struct grew to 104 rather than one
    // field quietly taking the other's bytes. pheno_flags now lives at 96.
    // The reject set narrows from "any nonzero" to "any UNKNOWN bit" -- a
    // pre-V-1b caller (zero-fill) is byte-identical, and a future flag still
    // cannot silently land on this kernel (the _pad_envp rationale).
    if (req->pheno_flags & ~(u32)SPAWN_PHENO_FLAGS_ALL) return -1;
    // ...which leaves 100 as the reserved slot. It is poison-checked for the
    // same reason _pad_envp is: the ONLY thing that keeps a future field from
    // being handed a caller's stale stack garbage is a kernel that refuses
    // nonzero today. Two independent branches have now each claimed a pad slot
    // and each shipped a caller that filled it; a slot nobody rejects is a
    // slot the next claimant inherits already-populated.
    if (req->_pad_spawn2 != 0)                         return -1;
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

    // argv_data is copied just before the synchronous body call below into a
    // KMALLOC'd buffer -- NOT a kernel-stack array. SYS_SPAWN_ARGV_DATA_MAX is
    // 64 KiB (the Go toolchain's compile/link command lines), far over the
    // 16 KiB kstack. validate_req already bounded argv_data_len <= the cap.

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

    // Copy argv_data into a kmalloc'd buffer (NOT a kernel-stack array --
    // SYS_SPAWN_ARGV_DATA_MAX is 64 KiB, over the 16 KiB kstack). The body
    // re-copies into its own kmalloc'd region (owned by the child's thunk)
    // synchronously before rfork returns, so this buffer only needs to outlive
    // the body call below; free it after. validate_req bounded argv_data_len.
    char *argv_kbuf = NULL;
    if (req.argv_data_len > 0) {
        argv_kbuf = kmalloc(req.argv_data_len, 0);
        if (!argv_kbuf) return -1;
        for (u32 i = 0; i < req.argv_data_len; i++) {
            u8 b = 0;
            if (uaccess_load_u8(req.argv_data_va + i, &b) != 0) {
                kfree(argv_kbuf);
                return -1;
            }
            argv_kbuf[i] = (char)b;
        }
    }
    s64 rc = (s64)sys_spawn_full_argv_budget_for_proc(
        p, name, (size_t)req.name_len,
        argv_kbuf, req.argv_data_len, req.argc,
        fds_kbuf, req.fd_count,
        (caps_t)req.cap_mask, req.perm_flags,
        set_identity, req.principal_id, req.primary_gid,
        supp_kbuf, supp_count,
        set_allowance ? &allow_kbuf : NULL,
        req.page_budget,                  // CL-5: 0 == inherit
        req.pheno_flags);                 // V-1b: 0 == inherit
    if (argv_kbuf) kfree(argv_kbuf);
    return rc;
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
// SYS_EXECVE — replace this Proc's program image in place (LINEAGE L-2,
// docs/LINEAGE.md section 5.2, invariant I-44).
// =============================================================================
//
// THE ORDERING IS THE DESIGN. Everything that can fail happens BEFORE anything
// the caller can observe changes:
//
//   1. copy the arguments in            -- from the OLD address space, which is
//                                          still the live one
//   2. resolve the program              -- I-28, in the caller's Territory
//   3. build a DETACHED address space   -- ELF parse, segment maps, stack, auxv
//   4. [commit]  swap + activate        -- infallible
//   5. rewrite the trapframe            -- the syscall's own eret starts it
//
// Step 3 is why exec_load_into exists. Building into a detached target rather
// than into the caller's own address space means a malformed ELF or an OOM
// leaves NOTHING to undo: the caller returns -errno with its image intact,
// which is what POSIX requires and what makes a failed exec debuggable. Linux
// reaches the same place from the other side (bprm->mm), having learned it the
// hard way -- its point of no return sits mid-exec and a failure past it kills
// the process.
//
// The multi-thread refusal at step 0 is documented at SYS_EXECVE in syscall.h.
//
// SPLIT AT THE ARGUMENT SHAPE (LINEAGE L-6a). Everything from step 2 down is
// the exec ITSELF and lives in sys_execve_core, which takes its arguments
// already in KERNEL memory. Two front ends produce that memory from two
// different user-side shapes:
//
//   sys_execve_handler  the native ABI -- (path_va, path_len, blob_va, ...),
//                       already concatenated, copied out of user memory here.
//   viv_execve          the Linux ABI -- a `char *const argv[]`, which must be
//                       WALKED and repacked into the blob. It has no user VA to
//                       hand over, which is precisely why the core cannot keep
//                       doing its own uaccess.
//
// This is the V-8 `sys_fstat_for_proc` discipline: one implementation of the
// decision, two front ends for the argument shape. The alternative -- a second
// execve body in the phenotype -- would be two implementations of the most
// consequential ordering in the file.
//
// The blob's PACKING CONTRACT is validated in the core rather than in the
// front ends, and that placement is load-bearing: exec_build_init_stack
// EXTINCTS on a NUL count that disagrees with argc, so a mis-built blob from a
// front end must be caught as -EINVAL before it reaches the loader. Validating
// it once, below both builders, means a bug in either is a clean error rather
// than a dead kernel.

static s64 sys_execve_core(struct exception_context *ctx,
                           const char *path, u64 path_len,
                           const char *argv_kbuf, u64 argv_data_len, u64 argc,
                           const char *env_kbuf, u64 env_data_len, u64 envc) {
    struct Thread *t = current_thread();
    if (!t)                                            return -(s64)T_E_INVAL;
    struct Proc *p = t->proc;
    if (!p || !p->as)                                  return -(s64)T_E_INVAL;

    if (path_len == 0 || path_len > SYS_OPEN_PATH_MAX) return -(s64)T_E_INVAL;
    if (argv_data_len > SYS_SPAWN_ARGV_DATA_MAX)       return -(s64)T_E_INVAL;
    if (argc > SYS_SPAWN_ARGV_MAX)                     return -(s64)T_E_INVAL;
    // The environment's own bounds answer T_E_2BIG, not T_E_INVAL, because a
    // caller acts on the difference: E2BIG says the request was well-formed and
    // too large, so splitting it will work. The argv bounds directly above
    // still answer EINVAL -- that is a LANDED native ABI (L-2a) whose error
    // code is its own deliberate decision, reserved to the #142 errno rollout
    // rather than changed as a side effect of adding envp. The asymmetry is
    // tracked, not accidental (docs/ERRORS.md says so at the T_E_2BIG row).
    if (env_data_len > EXEC_ENV_DATA_MAX)              return -(s64)T_E_2BIG;
    if (envc > EXEC_ENV_MAX)                           return -(s64)T_E_2BIG;
    // argc and argv_data_len are zero together or non-zero together: a count
    // with no bytes has nothing to point at, and bytes with no count would be
    // silently dropped rather than rejected. Same for the environment.
    if ((argc == 0) != (argv_data_len == 0))           return -(s64)T_E_INVAL;
    if ((envc == 0) != (env_data_len == 0))            return -(s64)T_E_INVAL;

    // The packing contract exec_build_init_stack relies on: exactly `argc`
    // NUL-terminated strings, the last byte a NUL. Validated for BOTH vectors
    // here, below every front end, because the builder EXTINCTS on a count that
    // disagrees -- so a mis-packed block from a front end must become a clean
    // -EINVAL before it reaches the loader, not a dead kernel.
    if (argv_data_len) {
        if (!argv_kbuf)                                return -(s64)T_E_INVAL;
        if (argv_kbuf[argv_data_len - 1] != '\0')      return -(s64)T_E_INVAL;
        u64 nuls = 0;
        for (u64 i = 0; i < argv_data_len; i++)
            if (argv_kbuf[i] == '\0') nuls++;
        if (nuls != argc)                              return -(s64)T_E_INVAL;
    }
    if (env_data_len) {
        if (!env_kbuf)                                 return -(s64)T_E_INVAL;
        if (env_kbuf[env_data_len - 1] != '\0')        return -(s64)T_E_INVAL;
        u64 nuls = 0;
        for (u64 i = 0; i < env_data_len; i++)
            if (env_kbuf[i] == '\0') nuls++;
        if (nuls != envc)                              return -(s64)T_E_INVAL;
    }

    // The multi-thread gate, before any allocation or FS work THIS function
    // does. A front end may have already copied its arguments in by now -- a
    // wasted copy on a path that is refusing, and nothing more. The gate lives
    // here rather than in each front end so it cannot be forgotten by one.
    if (!proc_exec_alone(p))                           return -(s64)T_E_AGAIN;

    // 2. Resolve in the CALLER's namespace (I-28: contained at root_spoor,
    //    per-component X-search, OEXEC on the leaf) -- the same helper every
    //    SYS_SPAWN_* uses, so exec and spawn cannot diverge on what is
    //    executable. Pins the Spoor; we clunk it below on every path.
    size_t exe_size = 0;
    struct Spoor *exe = exec_resolve_from_namespace(p, path, (size_t)path_len,
                                                    &exe_size);
    if (!exe) {
        // NOTE: argv_kbuf and env_kbuf are the CALLER's -- every front end frees
        // its own blobs on every path. The pre-L-6a body freed it here, when the
        // blob and the exec were the same function's; carrying those frees
        // across the split made it a double free, which is not a leak but a heap
        // corruption that surfaces in an unrelated allocation later. Do not
        // re-add them.
        // stalk's failure reason is not plumbed out yet (ER-x); ENOENT is the
        // honest majority answer and matches what SYS_OPEN reports today.
        return -(s64)T_E_NOENT;
    }

    // 3. Build the new image in a DETACHED address space. Nothing below this
    //    point until the commit touches `p`.
    // I-32 (A): exec builds a DETACHED space for the new image; cap it from
    // this Proc's authorization, the same source proc_alloc used. exec does
    // not change what the Proc is allowed, only what it is running.
    struct AddrSpace *nas = addrspace_alloc(p->page_budget);
    if (!nas) {
        spoor_clunk(exe);
        return -(s64)T_E_NOMEM;
    }

    u64 entry = 0, sp = 0;
    int rc = exec_load_into(nas, proc_resource_exempt(p), p, exe, exe_size,
                            path, (u32)path_len,
                            argv_kbuf, (u32)argv_data_len, (u32)argc,
                            env_kbuf, (u32)env_data_len, (u32)envc,
                            &entry, &sp);
    if (rc != 0) {
        // The target may be partially populated; we own its teardown, and are
        // its only reference (nothing has been published), so this unref is the
        // last one and drains whatever got mapped. The caller's own address
        // space was never touched, which is the whole point of building
        // detached.
        addrspace_unref(nas);
        spoor_clunk(exe);
        // DEGRADED, deliberately: POSIX says ENOEXEC here, and the errno
        // registry has no T_E_NOEXEC. docs/ERRORS.md is ABI-bearing and its
        // additions need signoff, so this reports EINVAL and the gap is tracked
        // rather than closed by fiat -- the same disposition T_E_SPIPE (#106)
        // carries. It matters at L-6: a shell reads ENOEXEC as "run it as a
        // script", so until the code exists a shell cannot make that
        // distinction.
        return -(s64)T_E_INVAL;
    }

    // 4. COMMIT. Infallible from here -- there is no path back to the caller's
    //    old image, and none is needed.
    proc_exec_replace(p, nas);

    // The Proc-side stamps the spawn path applies inside exec_setup_from_spoor.
    // They land HERE instead, after the commit, for the reason exec_load_into's
    // header gives: a name or /proc/<pid>/exe stamped before a load that then
    // failed would leave a live Proc advertising a program it is not running.
    if (exe->path)
        proc_set_name(p, exe->path->s, (size_t)exe->path->len);
    proc_set_exe_path(p, exe->path);

    spoor_clunk(exe);

    // #151: consume the close-on-exec flags. AFTER the commit, for two reasons
    // pulling the same way: a failed exec must leave the process unchanged, so
    // nothing that closes the caller's fds may run before the last thing that
    // can fail; and these closes SLEEP (a Spoor's Dev close hook sends a 9P
    // Tclunk), which the spoor_clunk directly above already establishes as legal
    // at this point. Linux places do_close_on_exec() after its own point of no
    // return for the first reason.
    //
    // BEFORE the trapframe rewrite below, so no instruction of the new image can
    // observe an fd that was supposed to be gone.
    (void)handle_close_on_exec(p);

    // 5. Rewrite the trapframe so this syscall's own eret enters the new image.
    //    KERNEL_EXIT (vectors.S) restores elr_el1 / sp_el0 / spsr_el1 from these
    //    fields, so setting them here IS the transition -- no separate
    //    userland_enter, and no window where the new address space is live but
    //    the PC still points into the old one.
    //
    //    Every GPR is zeroed to match userland_enter's contract (a fresh image
    //    must not inherit register contents, and x0 in particular would
    //    otherwise be read as this syscall's return value). SPSR 0 == EL0t with
    //    DAIF clear, identical to what userland_enter installs.
    for (int i = 0; i < 31; i++) ctx->regs[i] = 0;
    ctx->sp   = sp;
    ctx->elr  = entry;
    ctx->spsr = 0;
    return 0;   // ignored -- regs[0] was just zeroed on purpose
}

// The NATIVE front end: (path_va, path_len, argv_data_va, argv_data_len, argc),
// where the blob is already concatenated in user memory. Its whole job is
// step 1 of the ordering above -- get both arguments into kernel memory before
// the swap, because after it those user VAs mean something else entirely.
//
// IT PRESERVES THE ENVIRONMENT (#140), and that is what its ABI means rather
// than a shortcut: SYS_EXECVE takes no envp argument, so the request is
// execv/execvp's -- "run this program, keep my environment" -- which POSIX
// spells as passing the caller's own `environ`. The projection is staged HERE,
// before the commit, so a failure past this point cannot have disturbed the
// caller's /env; the frame gets a snapshot and the Env itself is untouched.
//
// A caller that wants to REPLACE the environment writes /env first (its own,
// or the child's after a fork) and then execs. There is no envp argument to
// add without changing a landed ABI, and none is needed: /env is the channel.
static s64 sys_execve_handler(struct exception_context *ctx) {
    u64 path_va       = ctx->regs[0];
    u64 path_len      = ctx->regs[1];
    u64 argv_data_va  = ctx->regs[2];
    u64 argv_data_len = ctx->regs[3];
    u64 argc          = ctx->regs[4];

    // Bounds the COPY depends on. The core re-checks these against its own
    // contract; here they gate how much user memory we are about to touch.
    if (path_len == 0 || path_len > SYS_OPEN_PATH_MAX) return -(s64)T_E_INVAL;
    if (argv_data_len > SYS_SPAWN_ARGV_DATA_MAX)       return -(s64)T_E_INVAL;
    if (!sys_validate_user_buf(path_va, path_len))     return -(s64)T_E_FAULT;
    if (argv_data_len &&
        !sys_validate_user_buf(argv_data_va, argv_data_len))
        return -(s64)T_E_FAULT;

    // The path goes on the stack (bounded by SYS_OPEN_PATH_MAX, the same
    // scratch SYS_OPEN uses); argv can be 64 KiB, so it is heap.
    char path[SYS_OPEN_PATH_MAX + 1];
    for (u64 i = 0; i < path_len; i++) {
        u8 b;
        if (uaccess_load_u8(path_va + i, &b) != 0)     return -(s64)T_E_FAULT;
        if (b == '\0')                                 return -(s64)T_E_INVAL;
        path[i] = (char)b;
    }
    path[path_len] = '\0';

    char *argv_kbuf = NULL;
    if (argv_data_len) {
        argv_kbuf = kmalloc((size_t)argv_data_len, 0);
        if (!argv_kbuf)                                return -(s64)T_E_NOMEM;
        for (u64 i = 0; i < argv_data_len; i++) {
            u8 b;
            if (uaccess_load_u8(argv_data_va + i, &b) != 0) {
                kfree(argv_kbuf);
                return -(s64)T_E_FAULT;
            }
            argv_kbuf[i] = (char)b;
        }
    }

    // Stage the caller's own environment for the new image's frame. `p` is
    // resolved here rather than trusting the core's, because the staging has to
    // happen before the core's commit and the core takes its arguments already
    // in kernel memory.
    struct Thread *t = current_thread();
    struct Proc *p   = t ? t->proc : NULL;
    char *env_kbuf = NULL;
    u32 env_len = 0, envc = 0;
    int es = exec_stage_env(p, &env_kbuf, &env_len, &envc);
    if (es != 0) {
        kfree(argv_kbuf);
        return (s64)es;                 // already a negative errno (-E2BIG/-ENOMEM)
    }

    s64 r = sys_execve_core(ctx, path, path_len, argv_kbuf, argv_data_len, argc,
                            env_kbuf, env_len, envc);
    kfree(argv_kbuf);   // no-op on NULL; on SUCCESS the strings are already
    kfree(env_kbuf);    // copied into the new image's stack by exec_load_into
    return r;
}

// =============================================================================
// SYS_RFORK -- LINEAGE L-3b (docs/LINEAGE.md section 5.4, invariant I-44).
//
// The EL0 surface for rfork -- both shapes since L-5 (RFPROC|RFMEM is vfork,
// RFPROC alone is fork) -- and the tree's first syscall that returns twice. The
// ABI is documented at SYS_RFORK in syscall.h; what follows is why this handler
// is shaped the way it is.
//
// It takes `ctx` for the same reason sys_execve_handler does, but inverted:
// execve REWRITES the frame so its own eret starts a new image; this one COPIES
// the frame so a second Thread can eret onto it. Either way the frame is the
// subject of the call, not a means of returning from it -- so the dispatch must
// not blindly store a result into regs[0] (see syscall_dispatch).
//
// All argument validation happens HERE, before rfork_forked, so that a kernel
// test can reach every rejection with a synthetic ctx and no address space.
// =============================================================================

// The core, taking its three arguments EXPLICITLY rather than reading them out
// of the frame -- because the frame is not the only place they come from.
//
// LINEAGE L-3d: a Linux `clone` arrives with a DIFFERENT register layout (x0
// flags, x1 stack, x2 parent_tid, x3 tls, x4 child_tid -- CONFIG_CLONE_BACKWARDS)
// and, on the call that matters, with x2/x3/x4 holding GARBAGE that musl's
// `__clone` moved there from registers `posix_spawn` never set. So the phenotype
// shell must supply translated values, not hand this function the raw frame.
// Splitting the read from the work is what lets both callers share ONE
// implementation of the gate below -- the V-8 `sys_fstat_for_proc` discipline.
//
// `ctx` is still required: it is the frame the CHILD's copy is made from.
static s64 sys_rfork_core(struct exception_context *ctx, unsigned flags,
                          u64 child_sp, u64 child_tls) {
    if (!ctx) return -(s64)T_E_INVAL;

    struct Thread *t = current_thread();
    if (!t)      return -(s64)T_E_INVAL;

    // LINEAGE L-5: both shapes now. RFPROC alone used to be refused here because
    // it would have handed the child a fresh EMPTY address space and then resumed
    // it at its parent's PC -- an instruction fetch fault on the first cycle. COW
    // exists (L-4b), rfork_internal clones for this shape, and the refusal has
    // become the thing standing between the tree and fork().
    if (flags != (unsigned)RFPROC && flags != (unsigned)(RFPROC | RFMEM))
        return -(s64)T_E_INVAL;

    // The SP rules are RFMEM's, not the fork's, and separating them is the whole
    // of this hunk. Under RFMEM the two Procs write the same physical stack, so a
    // shared SP corrupts both frames on the first push -- child_sp is mandatory
    // and may not be the caller's own.
    if (flags & RFMEM) {
        if (child_sp == 0)                   return -(s64)T_E_INVAL;

        // The one overlap case worth catching: handing the child the caller's OWN
        // live SP. This is a footgun-catcher, not a safety property -- an SP that
        // overlaps the parent's stack WITHOUT being equal to it is equally fatal
        // and is not detectable here (the parent's stack has no recorded extent).
        // The caller owns non-overlap, exactly as it does for a pthread stack;
        // this check just refuses the mistake that is free to see.
        if (child_sp == ctx->sp)             return -(s64)T_E_INVAL;
    }

    // Well-formedness binds any SP actually supplied, under either shape: an
    // unaligned or kernel-range SP is a bad value however the child got it.
    if (child_sp != 0) {
        if ((child_sp & 15u) != 0)           return -(s64)T_E_INVAL;
        if (child_sp >= UACCESS_USER_VA_TOP) return -(s64)T_E_INVAL;
    }

    // "0 means inherit", for the SP now as well as the TLS below -- and for a
    // fork it is not a convenience but the definition. A plain fork() has no
    // second stack to name: the child runs on its OWN COW copy at the SAME VA,
    // which is exactly `ctx->sp`. Resolving it HERE, in the layer that has the
    // caller in scope, is what keeps fork_frame_init at two unconditional edits
    // instead of teaching that primitive a "0 means keep" case.
    //
    // Under RFMEM this line is unreachable (child_sp == 0 was refused above), so
    // it cannot quietly hand a vfork child its parent's live SP.
    if (child_sp == 0)
        child_sp = ctx->sp;

    // "0 means inherit" is resolved here because this is the layer that has the
    // caller in scope. The LIVE register is the only correct source: the
    // caller's saved Context holds its last switch-OUT value, which is stale
    // while it is running.
    if (child_tls == 0)
        __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(child_tls));

    struct fork_context fc = {
        .frame     = ctx,
        .child_sp  = child_sp,
        .child_tls = child_tls,
    };

    int pid = rfork_forked(flags, &fc);
    if (pid < 0) return -(s64)T_E_AGAIN;

    // Only the PARENT reaches here. The child never returns from this call at
    // all: it is a separate Thread whose first switch-in lands in
    // thread_fork_trampoline and erets onto its own copy of `ctx` with
    // regs[0] == 0.
    return (s64)pid;
}

// The NATIVE reader: the SYS_RFORK ABI's register layout, and nothing else.
//
// Non-static so the argument gate is reachable from a kernel test with a
// synthetic frame (the sys_pci_claim_handler pattern -- the test carries its
// own extern decl). Every rejection in the core lands ahead of rfork_forked,
// which is what makes that coverage possible from kproc.
s64 sys_rfork_handler(struct exception_context *ctx) {
    if (!ctx) return -(s64)T_E_INVAL;
    return sys_rfork_core(ctx, (unsigned)ctx->regs[0], ctx->regs[1],
                          ctx->regs[2]);
}

// Store an `int` to a user VA, per-byte with fault fixup, scrubbing what was
// already written if a byte faults mid-store. Returns 0, or -1 with the
// partial range zeroed.
//
// THE SCRUB IS THE POINT, and it is why this is a helper rather than two loops.
// A wait that reaps and THEN faults on the status write has destroyed the only
// record of the child's exit code -- the caller sees a failure and must not
// also be able to read a torn half-status and believe it. The zeroing is
// best-effort (a store of 0 can fault too); the contract is only that the
// caller was told the write failed, so the buffer is not to be trusted either
// way. F240 established this; L-6b gave it a second caller, which is when a
// hand-copied loop would have started to drift.
static int sys_store_user_int(u64 va, int value) {
    const u8 *bytes = (const u8 *)&value;       // AArch64 is LE; `int` is 4 B
    for (u64 i = 0; i < sizeof(int); i++) {
        if (uaccess_store_u8(va + i, bytes[i]) != 0) {
            for (u64 j = 0; j < i; j++)
                (void)uaccess_store_u8(va + j, 0);
            return -1;
        }
    }
    return 0;
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

    int want_pid = (int)(s64)want_pid_u;   // -1 (any), a pid, 0/-N (pgrp)
    // Reject unknown/garbage flag bits (the full x1, before narrowing) so the
    // flag space stays clean for future additions and a fat-fingered flags
    // value fails loudly rather than silently behaving as blocking. Matches
    // the unknown-bit-reject discipline of the spawn / wstat surfaces.
    // PTY-1e added WAIT_UNTRACED/WAIT_CONTINUED to the wait_pid_for core + the
    // pgrp selectors on want_pid; the accepted-flag mask MUST admit them or the
    // job-control stop/continue reports are unreachable from EL0 (PTY-4: the
    // shell's WAIT_UNTRACED fg wait returned a spurious -1 -- the whole
    // ^Z-detects-a-stop path was closed at this gate).
    if (flags_u & ~(u64)(WAIT_WNOHANG | WAIT_UNTRACED | WAIT_CONTINUED))
                                                       return -1;
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

    // F240: a status write that faults AFTER the reap leaves the child gone
    // with no record of its exit code, so the partial range is scrubbed --
    // see sys_store_user_int.
    if (status_out_va != 0 && sys_store_user_int(status_out_va, status) != 0)
        return -1;

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
    bool   peer_renderer  = false;
    int    peer_pid       = 0;
    bool   peer_alive = proc_peer_snapshot_by_stripes(peer_stripes, &peer_caps,
                                                      &peer_principal, &peer_gid,
                                                      &peer_renderer, &peer_pid);

    out->stripes      = peer_stripes;
    out->caps         = peer_alive ? (u64)peer_caps : 0u;
    out->console      = peer_console ? 1u : 0u;
    out->alive        = peer_alive ? 1u : 0u;
    // A-1a: identity resolved fresh per query; a dead peer fail-closes to
    // NONE (the SrvConn captures only stripes + console immutably).
    out->principal_id = peer_alive ? peer_principal : PRINCIPAL_NONE;
    out->primary_gid  = peer_alive ? peer_gid       : GID_NONE;
    // cfg-3: the renderer-role stamp rides the same alive-gated walk as
    // caps — a dead/reaped peer fail-closes to 0 (never a stale grant).
    out->flags        = (peer_alive && peer_renderer)
                            ? SRV_PEER_FLAG_CONSOLE_RENDERER : 0u;
    // V-4a-0b: the peer's pid, same alive gate as caps/identity -- a dead peer
    // reports 0, never a pid a REUSED table entry now owns.
    out->pid          = peer_alive ? (u32)peer_pid : 0u;
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

// Write back the `revents` field (2 bytes at offset 6) of each pollfd, and
// NOTHING else -- `fd` and `events` are the caller's, and V-5c's ppoll depends
// on that: it polls a TRANSLATED fd, so rewriting the fd field would hand the
// guest a readiness-file handle where it wrote a socket.
//
// Per-byte uaccess_store_u8 with fault fixup; on a partial fault, scrub the
// bytes already written back to zero so userspace can never observe a torn
// revents (the sys_srv_peer_handler pattern). Returns 0, or -1 on that fault.
static int poll_writeback_revents(u64 fds_va, const struct pollfd *kfds, u64 nfds) {
    for (u64 i = 0; i < nfds; i++) {
        u64 rev_va  = fds_va + i * sizeof(struct pollfd)
                              + __builtin_offsetof(struct pollfd, revents);
        const u8 *rb = (const u8 *)&kfds[i].revents;
        for (u64 j = 0; j < sizeof(kfds[i].revents); j++) {
            if (uaccess_store_u8(rev_va + j, rb[j]) != 0) {
                // Scrub everything written so far -- this pollfd plus every
                // earlier one.
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
    return 0;
}

static s64 sys_poll_handler(u64 fds_va, u64 nfds_raw, u64 timeout_ms_raw) {
    struct Thread *t = current_thread();
    if (!t)                                                  return -1;
    struct Proc *p = t->proc;
    if (!p)                                                  return -1;

    // nfds bound — same as the testable core's check, but also the
    // stack-array bound on `kfds[]` below. Reject before touching
    // user-VA. POLL_MAX_NFDS (decoupled from PROC_HANDLE_MAX) keeps the
    // kfds[] frame bounded regardless of the open-fd table size.
    if (nfds_raw == 0 || nfds_raw > POLL_MAX_NFDS)          return -1;
    u64 nfds = nfds_raw;

    // User-VA range check on the entire pollfd[] array.
    u64 buf_bytes = nfds * sizeof(struct pollfd);
    if (!sys_validate_user_buf(fds_va, buf_bytes))           return -1;

    // Copy in: read all 8 bytes per pollfd. The kfds[] array is the
    // canonical fd+events the kernel operates on; this snapshot is
    // taken once at entry so the values can't change mid-sleep under
    // a concurrent userspace mutation.
    struct pollfd kfds[POLL_MAX_NFDS];
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

    if (poll_writeback_revents(fds_va, kfds, nfds) != 0) return -1;
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

// =============================================================================
// VIVARIUM V-1b — the syscall-entry phenotype branch (docs/VIVARIUM.md §4/§5).
// =============================================================================
//
// This is the piece V-2 built its tables for and deliberately left uncalled:
// "nothing here is wired into syscall_dispatch ... the dispatch branch would
// today be branching on a field that is provably always 0" (vivarium.h). V-7
// landed the container object that can now declare the field, so the branch
// becomes reachable and provable in the same chunk that gives it a producer.
//
// I-43 -- A PHENOTYPE CONFERS ABI SHAPE, NEVER AUTHORITY -- is what this code
// has to uphold, and the shape below is chosen so it holds BY CONSTRUCTION
// rather than by review:
//
//   * A TIER-1 row is a RENUMBER PERFORMED IN PLACE. We rewrite ctx->regs and
//     then FALL THROUGH into the native switch, so the call lands on the very
//     same `sys_*_handler` -- with the same capability gate, the same stalk
//     resolution, the same perm_check, the same resource charge -- that a
//     native caller reaches. There is no parallel implementation to keep in
//     sync, and therefore no way for a gate to be present on one path and
//     absent on the other.
//   * A TIER-2 row calls the SAME `sys_*_for_proc` core the native handler
//     calls (that is why sys_fstat_for_proc was extracted above). The only
//     phenotype-specific code is argument reshaping and struct conversion --
//     both PURE, both in kernel/vivarium.c, both unit-tested.
//
// WHY A MIS-DECLARED PHENOTYPE IS NOT A PRIVILEGE BUG, stated plainly because
// the declaration is deliberately ungated: every Linux number this table
// translates ALSO exists as a live native number (56 openat vs SYS_READDIR,
// 64 write vs SYS_CONSOLE_OPEN, 94 exit_group vs SYS_TTY_SIGNAL, ...). So a
// native Proc wrongly branded Linux does mis-decode its own numbers -- and
// that is exactly as far as it goes: the mis-decoded call still passes every
// gate the native caller would have faced, on the mis-brander's own Proc,
// with its own authority. It breaks itself and reaches nothing new. (The
// collision list is also why §12.1 rule 3 -- outside a vivarium the phenotype
// is ALWAYS native -- is load-bearing, and why the ELF byte may never decide.)
//
// FORWARD AT V-1b. §4's Option C sends a non-translatable call to a userspace
// supervisor, which is V-3. Until it exists, FORWARD and ENOSYS necessarily
// collapse to the same wire answer: -ENOSYS, the honest "this kernel cannot
// serve it" that §9's ladder promises ("ENOSYS is a supported outcome; a lie
// is not"). They are kept as SEPARATE case arms below so V-3's diff is the
// FORWARD arm alone.

// Measure a Linux NUL-terminated path in user memory. Linux hands a pointer;
// SYS_OPEN/SYS_STAT want an explicit length, and that measurement is the one
// impure part of the `openat`/`newfstatat` translations -- deliberately kept
// HERE rather than in vivarium.c, so the pure translators stay unit-testable
// with no kernel plumbing (vivarium.h "why the decide/build split").
//
// Bounded by SYS_OPEN_PATH_MAX: an unterminated path is a reject, never a
// runaway scan. Validates each byte's VA before loading it -- the length is
// unknown up front, so the usual validate-then-copy prologue cannot be used.
// Returns 0 with *len_out set (>= 1), or a negative -errno.
static s64 viv_measure_user_path(u64 path_va, u32 *len_out) {
    if (!len_out)                                    return -(s64)T_E_INVAL;
    for (u64 i = 0; i <= SYS_OPEN_PATH_MAX; i++) {
        if (!sys_validate_user_buf(path_va + i, 1))  return -(s64)T_E_FAULT;
        u8 b = 0;
        if (uaccess_load_u8(path_va + i, &b) != 0)   return -(s64)T_E_FAULT;
        if (b == '\0') {
            if (i == 0)                              return -(s64)T_E_NOENT;
            *len_out = (u32)i;
            return 0;
        }
    }
    // Linux answers ENAMETOOLONG here. That code is NOT in the errno registry
    // (docs/ERRORS.md is ABI-bearing and its additions need signoff), and the
    // native surface answers a bare -1 for the same input -- which a Linux
    // guest would read as EPERM, a wrong answer. -EINVAL is the honest
    // available one: an error, correctly attributed to the argument. The
    // ENAMETOOLONG registration is a named ER-x seam, shared with #83's
    // observation that SYS_OPEN has the same gap.
    return -(s64)T_E_INVAL;
}

// Convert a kernel `t_stat` into the 128-byte Linux layout and copy it out.
// The conversion itself is vivarium_stat_to_linux (pure, I-13-zeroed); this
// shell only moves bytes. Per-byte store, the sys_fstat_handler shape.
static s64 viv_stat_copy_out(u64 stat_va, const struct t_stat *ks) {
    if (!sys_validate_user_buf(stat_va, sizeof(struct viv_linux_stat)))
        return -(s64)T_E_FAULT;
    struct viv_linux_stat ls;
    vivarium_stat_to_linux(ks, &ls);
    const u8 *src = (const u8 *)&ls;
    for (u64 i = 0; i < sizeof(ls); i++) {
        if (uaccess_store_u8(stat_va + i, src[i]) != 0) return -(s64)T_E_FAULT;
    }
    return 0;
}

// uaccess.h exposes u8 and u32 primitives only (sys_note_mask_handler says the
// same where it hand-rolls its own 8-byte writeback), so the signal shells --
// which move u64 sigsets and handler pointers -- get byte-wise pairs here.
// Little-endian by construction, matching every other multi-byte marshalling in
// this file. Caller has already bounds-checked the whole span.
static int viv_load_u64(u64 va, u64 *out) {
    u64 v = 0;
    for (u32 i = 0; i < 8; i++) {
        u8 b = 0;
        if (uaccess_load_u8(va + i, &b) != 0) return -1;
        v |= (u64)b << (8u * i);
    }
    *out = v;
    return 0;
}

static int viv_store_u64(u64 va, u64 v) {
    for (u32 i = 0; i < 8; i++) {
        if (uaccess_store_u8(va + i, (u8)(v >> (8u * i))) != 0) return -1;
    }
    return 0;
}

// The NOTE_BIT_* numbering lives in `g_viv_notebits` (vivarium.c) -- V-6c moved
// it there when delivery became a SECOND consumer. Two files each carrying a
// `static` copy is the mirror-drift trap: each one's asserts would verify only
// itself.

// Get (or lazily create) this Proc's Linux disposition table.
//
// The allocation happens OUTSIDE every lock and is published with a
// compare-exchange, the 8a-2b debug_hw shape: peer threads racing their first
// rt_sigaction each bring a candidate, exactly one wins, the losers free theirs.
// Returns NULL only on OOM, which the caller reports rather than papering over.
static struct viv_sigtab *viv_sigtab_of(struct Proc *p) {
    struct viv_sigtab *tab = __atomic_load_n(&p->sigtab, __ATOMIC_ACQUIRE);
    if (tab) return tab;

    struct viv_sigtab *cand =
        (struct viv_sigtab *)kzalloc(sizeof(struct viv_sigtab), 0);
    if (!cand) return NULL;

    struct viv_sigtab *expected = NULL;
    if (__atomic_compare_exchange_n(&p->sigtab, &expected, cand, false,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        return cand;

    kfree(cand);            // lost the race; `expected` is the winner
    return expected;
}

// =============================================================================
// SOCKETS (V-5) -- the impure half. docs/VIVARIUM.md section 5.5.
//
// Every /net operation here goes through sys_open_kpath_for_proc, which is the
// SAME resolution core SYS_OPEN uses: the caller's Territory, the caller's
// per-component perm_check, the caller's omode-derived rights. That is what
// makes I-43 structural for sockets -- a translated socket call reaches
// exactly what the guest could reach by opening /net by hand, and a container
// whose territory has no /net gets a walk failure rather than a bypass.
// =============================================================================

// The socket table's lazy allocator -- viv_sigtab_of's twin, same CAS shape.
static struct viv_socktab *viv_socktab_of(struct Proc *p) {
    struct viv_socktab *tab = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
    if (tab) return tab;

    struct viv_socktab *cand =
        (struct viv_socktab *)kzalloc(sizeof(struct viv_socktab), 0);
    if (!cand) return NULL;

    // A KP_ZERO table has every entry state == VIV_SOCK_FREE, which is right,
    // but fd == 0 -- and 0 is a VALID fd. Stamp the free marker so a lookup for
    // fd 0 cannot match an unused entry. (find() also tests state, so this is
    // belt-and-braces; it costs one pass at first socket() and removes the need
    // for every future reader to remember the ordering.)
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) cand->s[i].fd = -1;

    struct viv_socktab *expected = NULL;
    if (__atomic_compare_exchange_n(&p->socktab, &expected, cand, false,
                                    __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
        return cand;

    kfree(cand);            // lost the race; `expected` is the winner
    return expected;
}

// Build "/net/<proto>/<tail>" or "/net/<proto>/<n>/<tail>" into `buf`.
// Returns the length, or 0 on overflow (which every caller treats as a
// refusal). Kernel-side only -- these paths are constructed, never echoed
// from the guest, so there is no injection surface: `proto` comes from a
// two-value enum and `n` is a u32 rendered as decimal.
static u32 viv_net_path(char *buf, u32 buflen, enum viv_net_proto proto,
                        bool have_n, u32 n, const char *tail) {
    const char *pd  = vivarium_net_proto_dir(proto);
    u32         off = 0;

    #define VIV_PUT(s) do {                                   \
        for (u32 i_ = 0; (s)[i_] != '\0'; i_++) {             \
            if (off >= buflen) return 0;                      \
            buf[off++] = (s)[i_];                             \
        }                                                     \
    } while (0)

    VIV_PUT("/net/");
    VIV_PUT(pd);
    if (off >= buflen) return 0;
    buf[off++] = '/';
    if (have_n) {
        char dec[11];
        u32  dn = 0;
        u32  v  = n;
        if (v == 0) dec[dn++] = '0';
        while (v > 0 && dn < sizeof(dec) - 1) { dec[dn++] = (char)('0' + (v % 10)); v /= 10; }
        for (u32 i = 0; i < dn; i++) {
            if (off >= buflen) return 0;
            buf[off++] = dec[dn - 1 - i];
        }
        if (off >= buflen) return 0;
        buf[off++] = '/';
    }
    VIV_PUT(tail);
    #undef VIV_PUT

    if (off >= buflen) return 0;   // room for the NUL the core does not need
    buf[off] = '\0';               //   but a mis-sized caller would
    return off;
}

// socket(domain, type, protocol) -> fd.
//
// Opens /net/<proto>/clone ORDWR. netd's clone idiom rebinds that fid onto the
// new connection's `ctl`, and that fid holds the connection's ONLY reference
// (slot_ref 0->1) -- so this fd must survive until connect() binds `data`, and
// dropping it early frees the connection. Reading it yields N.
static s64 viv_sock_socket(struct Proc *p, u64 domain, u64 type, u64 protocol) {
    enum viv_net_proto proto;
    s32                derr = 0;
    if (!vivarium_socket_decide(domain, type, protocol, &proto, &derr))
        return -(s64)derr;

    // The table is claimed BEFORE the open would otherwise succeed, so a full
    // table costs nothing on /net. (A claim needs the fd, so the order is:
    // check room, open, claim -- see the rollback below if the claim still
    // fails, which it cannot today but would if VIV_SOCK_MAX were raced.)
    struct viv_socktab *tab = viv_socktab_of(p);
    if (!tab) return -(s64)T_E_NOMEM;

    char path[64];
    u32  plen = viv_net_path(path, sizeof(path), proto, false, 0, "clone");
    if (plen == 0) return -(s64)T_E_INVAL;

    s64 fd = sys_open_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT, path, plen,
                                     2u /* ORDWR */);
    if (fd < 0) {
        // No /net in this territory, or netd refused the mint. ENETDOWN would
        // be a guess about which; the walk's own errno is the honest answer,
        // and a Linux caller handles ENOENT/EACCES from socket() poorly enough
        // that translating to ENETDOWN would hide a namespace mistake.
        return fd;
    }

    // Read N off the ctl fid -- the documented Plan 9 idiom, and netd serves it
    // (file_content: FK_CTL => push_dec(n)).
    u8  nbuf[16];
    s64 got = spoor_read_common(p, (hidx_t)fd, nbuf, sizeof(nbuf), false, 0);
    u32 n   = 0;
    if (got <= 0 || !vivarium_parse_conn_n((const char *)nbuf, (u32)got, &n)) {
        handle_close(p, (hidx_t)fd);
        return -(s64)T_E_IO;       // a /net that does not speak the idiom
    }

    if (!viv_socktab_claim(tab, (s32)fd, proto, n)) {
        handle_close(p, (hidx_t)fd);   // drops the ctl ref -> netd frees the conn
        return -(s64)T_E_MFILE;
    }
    return fd;
}

// connect(fd, addr, addrlen).
//
// Writes the dial verb to `ctl` (which the fd currently IS), opens `data`, and
// SWAPS the fd onto data. Ordering is load-bearing in both directions:
//   * data cannot be opened before the verb -- netd DEFERS the Rlopen until
//     ESTABLISHED (#257), so an early open would block on a socket that is not
//     going anywhere;
//   * ctl cannot be released before data is open -- it holds the connection's
//     only reference, and netd frees the connection at zero.
// handle_replace does both correctly by construction: it installs the new
// object first and releases the old one after.
static s64 viv_sock_connect(struct Proc *p, u64 fd_raw, u64 addr_va, u64 addrlen) {
    struct viv_socktab *tab = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
    struct viv_sock    *e   = viv_socktab_find(tab, (s32)(s64)fd_raw);
    if (!e)                       return -(s64)T_E_NOTSOCK;
    if (e->state == VIV_SOCK_CONNECTED) return -(s64)T_E_ISCONN;
    if (e->state == VIV_SOCK_LISTENING) return -(s64)T_E_ISCONN;

    // A CONSTRAINED bind cannot be honoured: netd's dial verb takes only the
    // REMOTE endpoint (its `!local` suffix is parsed and ignored), so a client
    // that bound a specific source port would silently get an ephemeral one --
    // exactly the mistranslation the argument domain forbids. Decline instead.
    //
    // An UNCONSTRAINED bind (0.0.0.0:0) asks for nothing netd is not already
    // doing, so it proceeds -- which is also why the table needs no `bound`
    // flag: "bound to anything" and "not bound" are the same request here.
    if (e->bound_port != 0 || e->bound_addr != 0) return -(s64)T_E_OPNOTSUPP;

    // Copy the sockaddr into kernel memory before looking at it -- the parse is
    // pure and must never read user memory twice (a peer thread rewriting it
    // between the family check and the address read is the classic TOCTOU).
    if (addrlen == 0 || addrlen > 128)                  return -(s64)T_E_INVAL;
    if (!sys_validate_user_buf(addr_va, addrlen))       return -(s64)T_E_FAULT;
    u8 sa[128];
    for (u64 i = 0; i < addrlen; i++) {
        if (uaccess_load_u8(addr_va + i, &sa[i]) != 0)  return -(s64)T_E_FAULT;
    }

    u8  ip4[4];
    u16 port = 0;
    if (!vivarium_sockaddr_in_parse(sa, addrlen, ip4, &port)) {
        // Wrong family is EAFNOSUPPORT; a short/degenerate address is EINVAL.
        // Telling them apart matters: a guest that gets EINVAL for an AF_INET6
        // address retries it.
        u16 fam = (u16)((u16)sa[0] | ((u16)sa[1] << 8));
        return (fam != 2) ? -(s64)T_E_AFNOSUPPORT : -(s64)T_E_INVAL;
    }

    char cmd[48];
    u32  clen = vivarium_net_cmd_ipport(cmd, sizeof(cmd), "connect", ip4, port);
    if (clen == 0)                return -(s64)T_E_INVAL;

    // A ctl verb is all-or-nothing: netd parses the whole buffer or rejects it,
    // so a SHORT write means a truncated command was accepted, not a slow one.
    // Unreachable today (clen <= 48, far under any negotiated msize) and checked
    // anyway, because "wrote some of a command" must never read as success.
    s64 w = spoor_write_common(p, (hidx_t)fd_raw, (const u8 *)cmd, clen, false, 0);
    if (w != (s64)clen)           return -(s64)T_E_CONNREFUSED;

    char path[64];
    u32  plen = viv_net_path(path, sizeof(path),
                             (enum viv_net_proto)e->proto, true, e->n, "data");
    if (plen == 0)                return -(s64)T_E_INVAL;

    // BLOCKS for TCP until ESTABLISHED (netd's deferred Rlopen). That is the
    // correct POSIX shape for a blocking connect(), and it is why SOCK_NONBLOCK
    // is refused at socket() rather than silently ignored.
    s64 dfd = sys_open_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT, path, plen,
                                      2u /* ORDWR */);
    if (dfd < 0)                  return -(s64)T_E_CONNREFUSED;

    // Take the data Spoor OUT of its temporary fd and put it in the socket's
    // fd. handle_get holds a ref across the move so the object cannot be freed
    // between the two steps.
    struct Handle dh;
    if (handle_get(p, (hidx_t)dfd, &dh) < 0) {
        handle_close(p, (hidx_t)dfd);
        return -(s64)T_E_IO;
    }
    // V-5d F5: check the KIND before the cast. handle_replace has a Spoor-only
    // gate, but it runs four lines below -- after spoor_ref would already have
    // incremented a refcount at an offset that is only a Spoor's by assumption.
    // Unreachable today (sys_open_kpath_for_proc allocates KOBJ_SPOOR
    // unconditionally, and no peer thread can swap the slot), so this is the
    // gate order made to match the header's claim rather than a live defect.
    if (dh.kind != KOBJ_SPOOR) {
        handle_put(&dh);
        handle_close(p, (hidx_t)dfd);
        return -(s64)T_E_IO;
    }

    // The ref handle_get took becomes the socket fd's ref; closing the
    // temporary fd drops the temporary's own ref, leaving exactly one.
    struct Spoor *dsp = (struct Spoor *)dh.obj;
    rights_t      dr  = dh.rights;
    spoor_ref(dsp);                       // the ref handle_replace will install
    handle_put(&dh);                      // release the borrowed one
    handle_close(p, (hidx_t)dfd);         // retire the temporary fd

    if (handle_replace(p, (hidx_t)fd_raw, KOBJ_SPOOR, dr, dsp) < 0) {
        spoor_clunk(dsp);
        return -(s64)T_E_IO;
    }

    e->state = VIV_SOCK_CONNECTED;
    return 0;
}

// Copy a guest sockaddr into kernel memory. Shared by connect/bind, and for the
// same reason both need it: the parse must never read user memory twice, or a
// peer thread could rewrite the family between the check and the address read.
static s64 viv_copy_sockaddr(u64 addr_va, u64 addrlen, u8 *out /* [128] */) {
    if (addrlen == 0 || addrlen > 128)            return -(s64)T_E_INVAL;
    if (!sys_validate_user_buf(addr_va, addrlen)) return -(s64)T_E_FAULT;
    for (u64 i = 0; i < addrlen; i++)
        if (uaccess_load_u8(addr_va + i, &out[i]) != 0) return -(s64)T_E_FAULT;
    return 0;
}

// bind(fd, addr, addrlen).
//
// REMEMBERED, NOT WRITTEN. netd has no `bind` ctl verb at all -- a local
// endpoint reaches it only as the argument of `announce` (a server) and is
// simply unavailable to a client. So bind records the request, and listen()
// spends it. That is the same shape the pouch boundary-line arrived at.
//
// The visible consequence is that a bind to a port already in use SUCCEEDS
// here and fails later at listen(), where netd's announce is refused. Linux
// reports EADDRINUSE from bind. That is a DEGRADED answer -- the error moves,
// it does not vanish -- and it is recorded as one in VIVARIUM.md section 9.
static s64 viv_sock_bind(struct Proc *p, u64 fd_raw, u64 addr_va, u64 addrlen) {
    struct viv_socktab *tab = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
    struct viv_sock    *e   = viv_socktab_find(tab, (s32)(s64)fd_raw);
    if (!e)                             return -(s64)T_E_NOTSOCK;
    if (e->state != VIV_SOCK_FRESH)     return -(s64)T_E_INVAL;

    u8  sa[128];
    s64 rc = viv_copy_sockaddr(addr_va, addrlen, sa);
    if (rc < 0) return rc;

    // The PERMISSIVE parse: bind(0.0.0.0:0) is an ordinary request, not the
    // malformed address connect() would refuse.
    u8  ip4[4];
    u16 port = 0;
    if (!vivarium_sockaddr_in_parse_any(sa, addrlen, ip4, &port)) {
        u16 fam = (u16)((u16)sa[0] | ((u16)sa[1] << 8));
        return (fam != 2) ? -(s64)T_E_AFNOSUPPORT : -(s64)T_E_INVAL;
    }

    e->bound_addr = ((u32)ip4[0] << 24) | ((u32)ip4[1] << 16)
                  | ((u32)ip4[2] << 8)  |  (u32)ip4[3];
    e->bound_port = port;
    return 0;
}

// listen(fd, backlog).
//
// Writes `announce` to ctl -- which the fd still IS, and stays: unlike connect,
// listen performs NO swap. The listening fd must remain ctl because that is
// what accept() re-walks from and what holds the listener's reference.
//
// `backlog` is dropped. netd owns its accept queue (depth 1 today) and offers
// no way to ask for another, so honouring the number is not possible; Linux
// itself treats the value as a hint and silently clamps it to a system
// maximum, so a caller cannot distinguish this from an ordinary clamp.
static s64 viv_sock_listen(struct Proc *p, u64 fd_raw, u64 backlog) {
    (void)backlog;
    struct viv_socktab *tab = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
    struct viv_sock    *e   = viv_socktab_find(tab, (s32)(s64)fd_raw);
    if (!e) return -(s64)T_E_NOTSOCK;

    s32 err = 0;
    if (!vivarium_listen_decide((enum viv_net_proto)e->proto,
                                (enum viv_sock_state)e->state,
                                e->bound_port, &err))
        return -(s64)err;      // err == 0 is the already-LISTENING success

    u8 ip4[4] = { (u8)(e->bound_addr >> 24), (u8)(e->bound_addr >> 16),
                  (u8)(e->bound_addr >> 8),  (u8)(e->bound_addr) };

    char cmd[48];
    u32  clen = vivarium_net_cmd_announce(cmd, sizeof(cmd), ip4, e->bound_port);
    if (clen == 0) return -(s64)T_E_INVAL;

    // All-or-nothing, exactly as connect's dial verb -- see the note there.
    s64 w = spoor_write_common(p, (hidx_t)fd_raw, (const u8 *)cmd, clen, false, 0);
    if (w != (s64)clen) {
        // netd refuses an announce for a port already listening, a socket
        // already open, or a port it will not take. EADDRINUSE is the one a
        // server actually branches on, and it is the likely cause.
        return -(s64)T_E_ADDRINUSE;
    }

    e->state = VIV_SOCK_LISTENING;
    return 0;
}

// accept(fd, addr, addrlen) / accept4(fd, addr, addrlen, flags).
//
// Unlike connect, this SWAPS NOTHING: it returns a NEW fd, and the fd it needs
// is the one sys_open_kpath_for_proc already hands back for `data`. So the
// sequence is a straight walk --
//
//   open(/net/tcp/N/listen)   BLOCKS; netd holds the Rlopen until a call lands,
//                             then REBINDS this fid onto the accepted
//                             connection's ctl and replies (net-3a)
//   read(that fd)          -> M, the accepted connection's number
//   open(/net/tcp/M/data)  -> the fd accept() returns
//   close(the listen fd)      M's ctl; data holds M's reference now
//
// The listener N is untouched throughout: netd re-arms it with a fresh socket
// during the swap, so it stays ANNOUNCED and the next accept() blocks again.
static s64 viv_sock_accept(struct Proc *p, u64 fd_raw, u64 addr_va,
                           u64 addrlen_va, u64 flags) {
    // accept4's flags are SOCK_NONBLOCK/SOCK_CLOEXEC -- refused for exactly the
    // reason socket() refuses them, and refused here rather than masked so a
    // guest asking for a non-blocking accepted socket does not silently get a
    // blocking one.
    if (flags != 0) return -(s64)T_E_INVAL;

    struct viv_socktab *tab = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
    struct viv_sock    *e   = viv_socktab_find(tab, (s32)(s64)fd_raw);
    if (!e)                                return -(s64)T_E_NOTSOCK;
    if (e->state != VIV_SOCK_LISTENING)    return -(s64)T_E_INVAL;

    // Ask BEFORE blocking. Past this point a real peer is connected, and
    // discovering a full table then would mean hanging up on it.
    if (!viv_socktab_has_room(tab))        return -(s64)T_E_MFILE;

    enum viv_net_proto proto = (enum viv_net_proto)e->proto;

    char path[64];
    u32  plen = viv_net_path(path, sizeof(path), proto, true, e->n, "listen");
    if (plen == 0) return -(s64)T_E_INVAL;

    // THE BLOCK. Propagate the open's own errno rather than flattening it:
    // netd answers ENOMEM when its deferred-accept table is full, and ENOMEM is
    // a documented accept(2) error that a server can act on.
    s64 lfd = sys_open_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT, path, plen,
                                      2u /* ORDWR */);
    if (lfd < 0) return lfd;

    // The fid is now the ACCEPTED connection's ctl, so reading it yields M.
    u8  nbuf[16];
    s64 got = spoor_read_common(p, (hidx_t)lfd, nbuf, sizeof(nbuf), false, 0);
    u32 m   = 0;
    if (got <= 0 || !vivarium_parse_conn_n((const char *)nbuf, (u32)got, &m)) {
        handle_close(p, (hidx_t)lfd);
        return -(s64)T_E_IO;
    }

    // Read the peer endpoint BEFORE the ctl fd goes away -- not because ctl is
    // needed for it (remote is walked from the root), but because a failure
    // here should leave the connection tidy, and ctl is the handle that tidies.
    u8  rip[4] = {0, 0, 0, 0};
    u16 rport  = 0;
    bool have_peer = false;
    if (addr_va != 0 && addrlen_va != 0) {
        char rpath[64];
        u32  rlen = viv_net_path(rpath, sizeof(rpath), proto, true, m, "remote");
        if (rlen != 0) {
            s64 rfd = sys_open_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                              rpath, rlen, 0u /* OREAD */);
            if (rfd >= 0) {
                u8  rbuf[32];
                s64 rgot = spoor_read_common(p, (hidx_t)rfd, rbuf, sizeof(rbuf),
                                             false, 0);
                if (rgot > 0)
                    have_peer = vivarium_parse_ipport((const char *)rbuf,
                                                      (u32)rgot, rip, &rport);
                handle_close(p, (hidx_t)rfd);
            }
        }
    }

    char dpath[64];
    u32  dlen = viv_net_path(dpath, sizeof(dpath), proto, true, m, "data");
    if (dlen == 0) {
        handle_close(p, (hidx_t)lfd);
        return -(s64)T_E_INVAL;
    }
    s64 dfd = sys_open_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT, dpath, dlen,
                                      2u /* ORDWR */);
    if (dfd < 0) {
        handle_close(p, (hidx_t)lfd);
        return -(s64)T_E_CONNABORTED;
    }

    // data now holds M's reference, so ctl is disposable -- the same ledger
    // connect() relies on, and the reason a Linux socket can be ONE fd when the
    // Plan 9 connection is two files.
    handle_close(p, (hidx_t)lfd);

    if (!viv_socktab_claim(tab, (s32)dfd, proto, m)) {
        handle_close(p, (hidx_t)dfd);      // frees M
        return -(s64)T_E_MFILE;
    }
    struct viv_sock *ne = viv_socktab_find(tab, (s32)dfd);
    if (ne) ne->state = VIV_SOCK_CONNECTED;   // born connected; the fd IS data

    // The peer address is a value-result parameter: *addrlen in is the caller's
    // buffer size, out is the FULL size, and a short buffer truncates. Failing
    // to write it would be a silent mistranslation -- a Linux server reads the
    // struct unconditionally -- so a failure here is reported, not ignored.
    // V-5d F2: EVERY exit from here unwinds the accept.
    //
    // By this point the accept has fully committed -- `dfd` is an open handle,
    // the socktab entry is claimed, and netd's connection M is live and held by
    // `dfd` alone. A bare `return -EFAULT` would leave the guest owning all
    // three and TELL IT NOTHING, since the fd number is the return value it
    // just lost: one handle (of PROC_HANDLE_MAX), one socktab entry (of
    // VIV_SOCK_MAX) and one netd slot (of netd's MAX_SLOTS, which is shared
    // across every /net client on the box) burned per call, reclaimable only by
    // Proc death. Linux unwinds here too -- __sys_accept4's move_addr_to_user
    // failure goes to out_fd: fput(newfile); put_unused_fd(newfd).
    //
    // The drop must precede the close: viv_socktab_drop is keyed on the fd, so
    // closing first would leave an entry pointing at a number the next
    // fd-creating call can be handed -- the stale-entry bug the close hook
    // exists to prevent, reintroduced from the other end.
    if (addr_va != 0 && addrlen_va != 0) {
        u32 cap = 0;
        if (!sys_validate_user_buf(addrlen_va, 4) ||
            uaccess_load_u32(addrlen_va, &cap) != 0)
            goto fault_unwind;

        // If `remote` could not be read we still return the fd -- the peer is
        // genuinely connected, and failing the accept would be worse than a
        // degraded address -- but the struct is written as 0.0.0.0:0 rather
        // than left holding the caller's stale bytes.
        static const u8 zero_ip[4] = {0, 0, 0, 0};
        u8  sa[16];
        u32 salen = vivarium_sockaddr_in_build(sa, sizeof(sa),
                                               have_peer ? rip : zero_ip,
                                               have_peer ? rport : 0);
        u32 wr = (cap < salen) ? cap : salen;
        if (wr != 0) {
            if (!sys_validate_user_buf(addr_va, wr)) goto fault_unwind;
            for (u32 i = 0; i < wr; i++)
                if (uaccess_store_u8(addr_va + i, sa[i]) != 0)
                    goto fault_unwind;
        }
        if (uaccess_store_u32(addrlen_va, salen) != 0) goto fault_unwind;
    }

    return dfd;

fault_unwind:
    viv_socktab_drop(tab, (s32)dfd);
    handle_close(p, (hidx_t)dfd);     // drops M's last ref -> netd frees it
    return -(s64)T_E_FAULT;
}

// ppoll(fds, nfds, tmo_p, sigmask, sigsetsize) -> ready count.
//
// The pollfd ARRAY needs no conversion: <thylacine/poll.h> is deliberately
// Linux-shaped -- 8 bytes, fd at 0, events at 4, revents at 6, and the same
// POLLIN/POLLOUT/POLLERR/POLLHUP values. So the only translation is the FD, and
// only for a socket.
//
// WHY A SOCKET FD CANNOT BE POLLED DIRECTLY. A /net socket's fd names
// `/net/<proto>/N/data`, an ORDINARY dev9p file -- and dev9p reports an ordinary
// file as POSIX always-ready, which is correct for a file and useless for a
// socket. netd publishes readiness on a SIBLING, `/net/<proto>/N/ready`, whose
// qid carries the reserved QTPOLL bit; dev9p.poll probes exactly that bit, and a
// poll on it becomes a non-consuming readiness Tread that netd answers or
// defers. So a poll on the socket's own fd would return "ready" instantly and
// defeat the wait -- the exact bug the pouch boundary-line hit at net-6b-3, and
// it is the same bug here for the same reason.
//
// THE READY FD IS OPENED PER CALL, NOT CACHED, AND THAT IS DELIBERATE. Caching
// it in the socktab (what pouch does) would put a handle the guest never asked
// for into the guest's OWN fd-number space, where the guest could close it --
// after which the cached number would name whatever object was allocated next,
// and poll would report a stranger's readiness as this socket's. In pouch that
// hazard does not exist, because there the ready fd IS a guest fd that the
// guest's own libc opened and tracks. Here the guest cannot see it, so it must
// not outlive the call.
//
// The transient fd is unobservable for EXACTLY the reason the socktab needs no
// lock -- a PHENO_LINUX Proc is single-threaded (clone is not a row), so nothing
// can look at the handle table while this one blocks in poll. Both properties
// evaporate together when process creation lands (VIVARIUM.md task #93); the
// caching option becomes available then only if the fd-space problem above is
// solved first.
// Poll a pollfd array on the guest's behalf: translate each /net socket fd to a
// freshly-opened readiness fd, run the native poll, then close every fd opened
// and PUT THE CALLER'S OWN fd NUMBERS BACK.
//
// The restore is load-bearing for pselect6 and merely tidy for ppoll, which is
// why it lives here rather than in either caller. ppoll writes back only the
// `revents` field, so a readiness handle left in `kfds[i].fd` would never reach
// the guest; pselect6 uses `kfds[i].fd` as the BIT INDEX to set in the caller's
// fd_set, so a left-behind readiness handle would report the wrong fd as ready.
// One shared helper, one invariant: on return, kfds[] holds the caller's fds.
static s64 viv_poll_translated(struct Proc *p, struct pollfd *kfds, u64 nfds,
                               s32 timeout_ms) {
    struct viv_socktab *tab = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
    s32  opened[POLL_MAX_NFDS];
    s32  orig[POLL_MAX_NFDS];
    bool any_socket = false;

    for (u64 i = 0; i < nfds; i++) {
        opened[i] = -1;
        orig[i]   = kfds[i].fd;
    }

    if (tab) {
        for (u64 i = 0; i < nfds; i++) {
            if (kfds[i].fd < 0) continue;          // caller-disabled entry
            struct viv_sock *e = viv_socktab_find(tab, kfds[i].fd);
            if (!e) continue;                      // an ordinary file: as-is

            char path[64];
            u32  plen = viv_net_path(path, sizeof(path),
                                     (enum viv_net_proto)e->proto, true, e->n,
                                     "ready");
            s64 rfd = (plen == 0)
                          ? -1
                          : sys_open_kpath_for_proc(p, SYS_WALK_OPEN_FROM_ROOT,
                                                    path, plen, 0u /* OREAD */);
            if (rfd < 0) {
                // The socket is in the table but its readiness file will not
                // open -- a dead connection, a /net that went away, or (V-5d F6)
                // a TRANSIENT shortage: the handle table full, or a kmalloc
                // shortfall inside the open. The three answer alike, and for the
                // third that is a real divergence -- Linux never turns a
                // resource shortage into EBADF on select. It is also partly
                // self-inflicted, since this design spends one guest-fd-space
                // handle per polled socket, so a guest polling near its ceiling
                // can drive itself into it. Left as-is deliberately: the fix is
                // to stop consuming guest fd numbers at all, which is the same
                // change #98 needs (a poll core that holds Spoors), and
                // splitting the arm now would encode the fd-space design it
                // should replace. POLLNVAL
                // is the POSIX answer for an fd that cannot be polled, and it is
                // per-pollfd: one broken socket must not fail the whole call for
                // the fds beside it. (pselect6 then turns that POLLNVAL into a
                // whole-call EBADF, which is select's own contract -- the split
                // belongs to the caller, not here.)
                kfds[i].fd = -1;
                continue;
            }
            opened[i]  = (s32)rfd;
            kfds[i].fd = (s32)rfd;
            any_socket = true;
        }
    }

    // A ZERO TIMEOUT STILL NEEDS A MOMENT, and the reason is a property of the
    // object rather than a shortcut. Readiness for a /net socket lives in netd,
    // one RPC away: dev9p's .poll SUBMITS an async probe and answers from a
    // cache that the freshly-opened `ready` fd does not yet have. So a strict
    // zero-timeout scan would report "nothing ready" for a socket that is
    // plainly writable -- and a caller polling with timeout 0 in a loop would
    // never make progress at all.
    //
    // Giving the probe a small budget changes the LATENCY, not the ANSWER: what
    // comes back is netd's real verdict rather than an approximation of it. If
    // the probe misses even this, the call reports not-ready and the caller
    // retries, which is the safe direction. A caller-supplied timeout is never
    // touched -- only the literal 0 is widened, and only when a socket is
    // actually in the array.
    //
    // This is a mitigation, not a closure (task #98): a slow or loaded path can
    // still miss the budget. Closing it needs either a poll core that holds
    // Spoors rather than fd indices (so the ready fd can be cached OUTSIDE the
    // guest's fd-number space) or a synchronous readiness query on dev9p_poll.
    if (timeout_ms == 0 && any_socket) timeout_ms = VIV_PPOLL_PROBE_MS;

    // V-5d F1: COMPACT AWAY THE CALLER-DISABLED ENTRIES BEFORE POLLING.
    //
    // Linux and the native poll disagree about a negative fd, and the
    // disagreement is total. poll(2): "If fd is negative, then the
    // corresponding events field is ignored and the revents field returns
    // zero" -- the entry is INERT and contributes nothing to the count. That is
    // how every fixed-array event loop disables a slot without compacting.
    // Thylacine's poll says the opposite and documents it (poll.h: "negative =>
    // POLLNVAL"): poll_scan_one returns 1 for such an entry, which is a
    // perfectly good NATIVE ABI and is not Linux's.
    //
    // Passing them through is therefore not a pass-through at all. ANY
    // caller-disabled entry makes ready_count > 0 on the first scan, the native
    // poll takes its fast path, and a ppoll asked to block forever RETURNS AT
    // ONCE with POLLNVAL on exactly the slots the caller had switched off -- a
    // hard spin, plus a revents a robust event loop reads as "this fd died".
    // (It would also defeat the #98 probe budget: the fast path fires before
    // VIV_PPOLL_PROBE_MS can be spent, so a socket beside a disabled slot would
    // report not-ready forever.)
    //
    // Subtracting them from the result afterwards would fix the count and the
    // revents and NOT the blocking, so they must not reach the native poll at
    // all. `orig` is the discriminator that makes this exact: orig[i] < 0 is
    // the CALLER's disable, while orig[i] >= 0 with kfds[i].fd < 0 is OURS
    // (a readiness file that would not open), and that one is still owed its
    // POLLNVAL. Compaction only ever moves an entry DOWN, so src[j] >= j and
    // the scatter can run high-to-low in place without clobbering.
    u32 src[POLL_MAX_NFDS];
    u32 dense = 0;
    for (u64 i = 0; i < nfds; i++) {
        if (orig[i] < 0) continue;              // inert, per POSIX
        if (dense != (u32)i) kfds[dense] = kfds[i];
        src[dense] = (u32)i;
        dense++;
    }

    s64 result;
    if (dense == 0) {
        // Every entry disabled: there is nothing to wait ON, but there is still
        // a timeout to wait FOR -- the same shape as nfds == 0, so it routes to
        // the same primitive rather than returning early.
        result = sys_poll_sleep_for(timeout_ms);
    } else {
        result = sys_poll_for_proc(p, kfds, dense, timeout_ms);

        // Scatter the answers back to the slots the caller used. High-to-low:
        // src[j] >= j, so every write lands at or above the read.
        for (u32 j = dense; j-- > 0; ) kfds[src[j]] = kfds[j];
    }

    // The inert entries report zero, which is the whole of Linux's contract for
    // them. Their fd is restored by the loop below like any other.
    for (u64 i = 0; i < nfds; i++) {
        if (orig[i] < 0) kfds[i].revents = 0;
    }

    // Close what we opened and restore what the caller wrote -- on EVERY path,
    // including the error one, because the transient fds must not outlive the
    // call and the caller's array must not carry our handles back out.
    for (u64 i = 0; i < nfds; i++) {
        if (opened[i] >= 0) {
            handle_close(p, (hidx_t)opened[i]);
            opened[i] = -1;
        }
        kfds[i].fd = orig[i];
    }

    return result;
}

// Read a Linux `struct timespec *` into a native millisecond timeout, with NULL
// meaning "block indefinitely" (the native negative timeout). Shared by ppoll
// and pselect6, which take the identical argument.
static bool viv_timeout_from_timespec(u64 tmo_va, s32 *out_ms, s32 *out_err) {
    if (tmo_va == 0) { *out_ms = -1; return true; }

    // Linux's struct timespec is two 8-byte fields; t_timespec matches it
    // field-for-field, so this is a copy rather than a conversion.
    struct t_timespec ts;
    if (!sys_validate_user_buf(tmo_va, sizeof(ts)) ||
        uaccess_copy_in(&ts, tmo_va, sizeof(ts)) != 0) {
        *out_err = (s32)T_E_FAULT;
        return false;
    }
    return vivarium_timespec_to_ms(ts.tv_sec, ts.tv_nsec, out_ms, out_err);
}

static s64 viv_ppoll(struct Proc *p, u64 fds_va, u64 nfds, u64 tmo_va,
                     u64 sigmask_va) {
    s32 err = 0;
    if (!vivarium_ppoll_decide(nfds, sigmask_va, &err)) return -(s64)err;

    s32 timeout_ms = -1;
    if (!viv_timeout_from_timespec(tmo_va, &timeout_ms, &err)) return -(s64)err;

    // nfds == 0 is Linux's "sleep for the timeout". The native poll rejects it
    // (deliberately -- see sys_poll_sleep_for), so it routes to the sleep rather
    // than declining as it did before V-5c-2.
    if (nfds == 0) return sys_poll_sleep_for(timeout_ms);

    u64 buf_bytes = nfds * sizeof(struct pollfd);
    if (!sys_validate_user_buf(fds_va, buf_bytes))      return -(s64)T_E_FAULT;

    struct pollfd kfds[POLL_MAX_NFDS];
    u8 *kbytes = (u8 *)kfds;
    for (u64 i = 0; i < buf_bytes; i++) {
        if (uaccess_load_u8(fds_va + i, &kbytes[i]) != 0) return -(s64)T_E_FAULT;
    }

    s64 result = viv_poll_translated(p, kfds, nfds, timeout_ms);

    if (result < 0) return result;
    if (poll_writeback_revents(fds_va, kfds, nfds) != 0) return -(s64)T_E_FAULT;
    return result;
}

// pselect6(nfds, readfds, writefds, exceptfds, tmo, sigmask_and_size)
//   -> count of ready BITS.
//
// The conversion is entirely in kernel/vivarium.c and unit-driven; this shell is
// uaccess plus the poll call. The only judgement here is the ORDER, which is
// chosen so that a call destined to fail does so before touching anything:
// decide, then timeout, then read the sets, then translate-and-poll, and only
// then write the sets back.
//
// THE SIXTH ARGUMENT IS A POINTER TO A PAIR, not a mask. aarch64 caps a syscall
// at six registers and pselect6 needs seven things, so Linux packs the last two
// into `struct { const sigset_t *ss; size_t ss_len; }` and passes its address.
// A NULL sixth argument is unambiguously "no signal mask"; anything non-NULL is
// declined without being dereferenced, since a non-NULL pair holding a NULL ss
// is a distinction we do not need to make in order to say no.
static s64 viv_pselect6(struct Proc *p, u64 nfds_arg, u64 rd_va, u64 wr_va,
                        u64 ex_va, u64 tmo_va, u64 sigmask_va) {
    s32 err   = 0;
    u32 nfds  = 0;
    if (!vivarium_pselect6_decide(nfds_arg, sigmask_va, &nfds, &err))
        return -(s64)err;

    s32 timeout_ms = -1;
    if (!viv_timeout_from_timespec(tmo_va, &timeout_ms, &err)) return -(s64)err;

    // Copy in only the bytes covering [0, nfds) -- see vivarium_fdset_bytes.
    // Zeroed in full so the scan and the write-back both see defined bytes even
    // where the caller's object was shorter than the whole 128.
    u8 rd[VIV_FD_SET_BYTES], wr[VIV_FD_SET_BYTES], ex[VIV_FD_SET_BYTES];
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) { rd[i] = 0; wr[i] = 0; ex[i] = 0; }

    u32 set_bytes = vivarium_fdset_bytes(nfds);
    struct { u64 va; u8 *buf; } sets[3] = {
        { rd_va, rd }, { wr_va, wr }, { ex_va, ex },
    };
    if (set_bytes != 0) {
        for (u32 s = 0; s < 3; s++) {
            if (sets[s].va == 0) continue;
            if (!sys_validate_user_buf(sets[s].va, set_bytes) ||
                uaccess_copy_in(sets[s].buf, sets[s].va, set_bytes) != 0)
                return -(s64)T_E_FAULT;
        }
    }

    struct pollfd kfds[POLL_MAX_NFDS];
    u32 count = 0;
    if (!vivarium_fdset_to_pollfds(rd_va ? rd : NULL, wr_va ? wr : NULL,
                                   ex_va ? ex : NULL, nfds, kfds,
                                   POLL_MAX_NFDS, &count, &err))
        return -(s64)err;

    u32 bits = 0;
    if (count == 0) {
        // No fd contributes: `select(0, NULL, NULL, NULL, &tv)`, the classic
        // portable sleep, and equally `select(n, ...)` with every set empty.
        // Nothing to poll -- but there is still something to write back.
        s64 slept = sys_poll_sleep_for(timeout_ms);
        if (slept < 0) return slept;

        // Linux writes the sets back on this path too: it copies FDS_BYTES(n)
        // bytes out of a buffer it zeroed, so a bit the caller set ABOVE nfds --
        // in range of the COPY even though out of range of the SCAN -- comes
        // home clear. Falling through to the shared write-back with the buffers
        // zeroed reproduces that. (The count > 0 path gets the same zeroing for
        // free inside vivarium_pollfds_to_fdset.) A well-formed caller never
        // sets such a bit; the cost of matching anyway is three memsets.
        for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) {
            rd[i] = 0; wr[i] = 0; ex[i] = 0;
        }
    } else {
        s64 result = viv_poll_translated(p, kfds, count, timeout_ms);
        if (result < 0) return result;

        if (!vivarium_pollfds_to_fdset(kfds, count, rd_va ? rd : NULL,
                                       wr_va ? wr : NULL, ex_va ? ex : NULL,
                                       &bits, &err))
            return -(s64)err;
    }

    // Write back last. A fault here has already consumed the wait, but the
    // caller's sets are its only channel for the answer, so there is nothing to
    // report but EFAULT -- the same position every copy-out syscall is in.
    for (u32 s = 0; s < 3; s++) {
        if (sets[s].va == 0) continue;
        for (u32 i = 0; i < set_bytes; i++) {
            if (uaccess_store_u8(sets[s].va + i, sets[s].buf[i]) != 0)
                return -(s64)T_E_FAULT;
        }
    }

    return (s64)bits;
}

// Measure a NUL-terminated user string, bounded by `max` bytes not counting the
// NUL. Unlike viv_measure_user_path, an EMPTY string is LEGAL here: `argv[i]`
// may be "" and a shell produces them (`cmd ""`), so a zero length is a value
// rather than an error.
static s64 viv_measure_user_str(u64 va, u32 max, u32 *len_out) {
    if (!len_out) return -(s64)T_E_INVAL;
    for (u32 i = 0; i <= max; i++) {
        if (!sys_validate_user_buf(va + i, 1))  return -(s64)T_E_FAULT;
        u8 b = 0;
        if (uaccess_load_u8(va + i, &b) != 0)   return -(s64)T_E_FAULT;
        if (b == '\0') { *len_out = i; return 0; }
    }
    // Linux answers E2BIG, and since #140 so do we -- the registry carries the
    // value now (appended under the #142 signoff by the commit that wired its
    // first consumer). This helper's only caller is the vivarium execve, so the
    // change is confined to a Linux-ABI row, where POSIX alignment is the
    // stated goal; the NATIVE SYS_EXECVE bounds still answer EINVAL, which is
    // its own deliberate ABI decision reserved to the #142 rollout.
    return -(s64)T_E_2BIG;
}

// Walk a Linux NULL-terminated `char *const v[]` in the guest's memory and
// repack it into ONE kernel blob of concatenated NUL-terminated strings plus a
// count -- the shape sys_execve_core takes.
//
// ONE implementation for argv and envp. They are the same walk with different
// bounds, and #140 is a standing demonstration of what writing it twice costs:
// the envp half of the frame builder was a copy of the argv half, so the bug
// had two homes and fixing either alone would have left the other.
//
// On success *blob_out is a kmalloc'd buffer the CALLER frees -- NULL when the
// vector is absent or empty, which is not an error but the honest answer to
// `execve(p, argv, NULL)`: Linux gives that image an EMPTY environment, not an
// inherited one.
static s64 viv_pack_strv(u64 vec_va, u32 max_count, u32 max_data,
                         char **blob_out, u64 *len_out, u64 *count_out) {
    *blob_out  = NULL;
    *len_out   = 0;
    *count_out = 0;
    if (!vec_va) return 0;

    // PASS 1 -- count the elements and total their lengths. Only a total is
    // kept, not a per-element array: 512 lengths would be 2 KiB of kernel
    // stack next to the 1 KiB path buffer, and pass 2 does not need them (see
    // its cap below).
    u64 count = 0, total = 0;
    for (;;) {
        if (count > (u64)max_count)             return -(s64)T_E_2BIG;
        u64 sv = vec_va + count * 8u;
        if (!sys_validate_user_buf(sv, 8))      return -(s64)T_E_FAULT;
        u64 sp_ptr = 0;
        if (viv_load_u64(sv, &sp_ptr) != 0)     return -(s64)T_E_FAULT;
        if (sp_ptr == 0) break;                 // the NULL terminator
        u32 sl = 0;
        s64 r = viv_measure_user_str(sp_ptr, max_data, &sl);
        if (r != 0) return r;
        total += sl;
        if (total + count + 1u > (u64)max_data) return -(s64)T_E_2BIG;
        count++;
    }
    if (count == 0) return 0;

    u64 blob_len = total + count;               // one NUL per string
    char *blob = kmalloc((size_t)blob_len, 0);
    if (!blob)                                  return -(s64)T_E_NOMEM;

    // PASS 2 -- copy. Every write is bounded by pass 1's measurement, NOT by
    // re-finding a NUL, because the strings live in the guest's own memory and
    // nothing here holds it still. A peer that lengthens a string between the
    // passes must not be able to overrun the buffer pass 1 sized; the same
    // submit-time-snapshot discipline I-30 applies to a shared ring. (No such
    // peer exists today -- a vfork parent is suspended and CLONE_THREAD is not
    // a row -- but the bound costs nothing and removes the class rather than
    // resting on that.)
    //
    // `cap` reserves one byte for each NUL still owed, so the terminator write
    // below can never be the byte that overflows.
    u64 w = 0;
    for (u64 i = 0; i < count; i++) {
        u64 sv = vec_va + i * 8u;
        u64 sp_ptr = 0;
        if (!sys_validate_user_buf(sv, 8) || viv_load_u64(sv, &sp_ptr) != 0) {
            kfree(blob);
            return -(s64)T_E_FAULT;
        }
        u64 cap = blob_len - (count - i);
        u64 n = 0;
        while (w < cap) {
            u8 b = 0;
            if (!sys_validate_user_buf(sp_ptr + n, 1) ||
                uaccess_load_u8(sp_ptr + n, &b) != 0) {
                kfree(blob);
                return -(s64)T_E_FAULT;
            }
            if (b == '\0') break;               // shortened since pass 1
            blob[w++] = (char)b;
            n++;
        }
        blob[w++] = '\0';                       // exactly `count` NULs, always
    }

    *blob_out  = blob;
    *len_out   = w;                             // == total + count unless a
                                                // string shortened mid-walk
    *count_out = count;
    return 0;
}

// execve(path, argv, envp) -- LINEAGE L-6a.
//
// The translation is the ARGUMENT SHAPE: Linux passes a NULL-terminated array
// of pointers to NUL-terminated strings; SYS_EXECVE takes one concatenated
// blob plus a count. So this walks argv and repacks it, which is the whole
// reason sys_execve_core exists (there is no user VA here to hand over).
//
// ENVP IS HONOURED SINCE #140. It used to be DECLINED when non-empty, under
// the argument-domain rule (V-2b section 4): a T2 row admits only argument
// values whose effect the native mechanism reproduces EXACTLY, and Linux's
// envp means "the new image's environment is exactly this", which the kernel
// could not produce at any layer -- the frame builder wrote a lone NULL for
// envp in both of its shapes, so a new image's `environ` was empty however the
// kernel was asked.
//
// The decline was also a DETECTOR, and it detected: at #151 a busybox ash
// spawning an external command tripped it with envc=2 and env0='SHLVL=1'. ash
// SYNTHESIZES SHLVL and PWD itself, so its envp is non-empty even starting
// from an empty environment and no container configuration avoids the arm --
// which made this the L-6c gate's last blocker and #140 the answer. The fix
// went where the detector said it would: the frame, not a weakening here.
static s64 viv_execve(struct exception_context *ctx, u64 path_va, u64 argv_va,
                      u64 envp_va) {
    u32 path_len = 0;
    s64 m = viv_measure_user_path(path_va, &path_len);
    if (m != 0) return m;
    char path[SYS_OPEN_PATH_MAX + 1];
    for (u32 i = 0; i < path_len; i++) {
        u8 b = 0;
        if (uaccess_load_u8(path_va + i, &b) != 0)  return -(s64)T_E_FAULT;
        path[i] = (char)b;
    }
    path[path_len] = '\0';

    // Both vectors through the same packer, with their own bounds. argv is
    // packed first only because a failure there costs less work; neither order
    // is required.
    char *blob = NULL;
    u64 blob_len = 0, argc = 0;
    s64 r = viv_pack_strv(argv_va, SYS_SPAWN_ARGV_MAX, SYS_SPAWN_ARGV_DATA_MAX,
                          &blob, &blob_len, &argc);
    if (r != 0) return r;

    char *env = NULL;
    u64 env_len = 0, envc = 0;
    r = viv_pack_strv(envp_va, EXEC_ENV_MAX, EXEC_ENV_DATA_MAX,
                      &env, &env_len, &envc);
    if (r != 0) {
        kfree(blob);
        return r;
    }

    r = sys_execve_core(ctx, path, path_len, blob, blob_len, argc,
                        env, env_len, envc);
    kfree(blob);
    kfree(env);
    return r;
}

// wait4(pid, wstatus, options, rusage): x0..x3. LINEAGE L-6b.
//
// The row that lets a guest REAP what L-6a let it create. `wait_pid_for` is
// already a POSIX waitpid, so this is a MAP -- and the map is the work, because
// the option words look interchangeable and are not (vivarium.h has the
// collision in full).
static s64 viv_wait4(u64 pid_u, u64 wstatus_va, u64 options, u64 rusage_va) {
    struct viv_wait_opts o;
    if (vivarium_wait4_decide(options, rusage_va, &o) != VIV_TRANSLATED)
        return -(s64)T_E_NOSYS;                 // out of domain -> V-3 forwards

    // THE ONLY PLACE THAT NAMES BOTH VOCABULARIES. The pure layer said what was
    // asked in Linux's terms; the translation to Thylacine's happens here, and
    // the third line is the one that matters -- Linux's WCONTINUED is bit 8 and
    // Thylacine's WAIT_CONTINUED is bit 4.
    int flags = 0;
    if (o.nohang)    flags |= WAIT_WNOHANG;
    if (o.untraced)  flags |= WAIT_UNTRACED;
    if (o.continued) flags |= WAIT_CONTINUED;

    // wait_pid_for applies the packed encoding IFF a PTY-1e flag was passed
    // (proc.h) -- a plain wait keeps the RAW exit status for pre-PTY callers.
    // Linux always wants packed, so we pack exactly when the kernel will not.
    //
    // THIS MUST BE DECIDED BEFORE THE CALL, and derived from the word one line
    // above rather than from `options`: the returned value cannot be classified
    // after the fact, because a raw exit status of 5247 and a packed
    // WAIT_STATUS_STOPPED are both 0x147f. Only what we ASKED for tells them
    // apart.
    const bool kernel_packs = (flags & (WAIT_UNTRACED | WAIT_CONTINUED)) != 0;

    // Validate up-front, as the native handler does: a wait that reaps and THEN
    // faults on the status write has destroyed the child's exit code with
    // nothing left to report it.
    if (wstatus_va != 0 && !sys_validate_user_buf(wstatus_va, sizeof(int)))
        return -(s64)T_E_FAULT;

    // `pid` passes straight through: wait_pid_for's selectors ARE Linux's
    // (-1 any / >0 that child / 0 the caller's group / <-1 the group -pid).
    // Narrowed through s32 so a caller that left x0 zero-extended rather than
    // sign-extended is read identically -- the VIV_AT_FDCWD hazard, which is a
    // property of every `int` parameter in this ABI and not special to dirfd.
    int status = 0;
    int reaped = wait_pid_for((int)(s32)(u32)pid_u, flags, &status);

    // -1 covers BOTH of wait_pid_for's failure conditions: no matching child,
    // and a #811 death-interrupted sleep. ECHILD for both is exact rather than
    // lossy -- the death path returns through the sync-from-EL0 tail where
    // el0_return_die_check is NORETURN on the die branch (vectors.S), so a
    // group-terminating Thread never carries a value back to EL0. There is no
    // observer that could tell the two apart.
    if (reaped < 0)  return -(s64)T_E_CHILD;
    if (reaped == 0) return 0;                  // WNOHANG, nothing ready

    if (wstatus_va != 0) {
        if (!kernel_packs) status = WAIT_STATUS_EXITED(status);
        if (sys_store_user_int(wstatus_va, status) != 0)
            return -(s64)T_E_FAULT;
    }
    return (s64)reaped;
}

// writev (#150). The row the L-6c gate was blocked on -- busybox's `echo` writes
// through it, so with no translator the shell ran perfectly and printed nothing.
//
// LOOPS THE EXISTING BYTE-I/O CORE rather than growing a vectored one. Each
// entry goes through sys_write_handler, which is the whole audited staging path
// -- the weft fast-path, the CF-3 two-tier bounce, the SYS_RW_MAX clamp, the #100
// errno translation -- so this function adds a decode and nothing else. A
// vectored core would have had to reproduce all of it.
//
// TWO PASSES, and the reason is Linux's ERROR semantics rather than memory
// safety. Linux validates the whole array up front (import_iovec) and answers
// EINVAL/EFAULT having written NOTHING; a single-pass loop that validated entry
// k just before writing it would leave entries 0..k-1 already written when it
// found a bad one. Memory safety does not depend on this -- pass 2 re-validates
// every buffer through sys_write_handler's own checks -- so the re-read is
// benign: a peer thread rewriting the array between passes yields values that
// are themselves validated before use, and the outcome degrades to a short
// write, which is a legal writev result.
//
// The storage cost is O(1) deliberately. UIO_MAXIOV is 1024 and an iovec is 16
// bytes, so buffering the array would want 16 KiB -- the entire kernel stack.
static s64 viv_writev(u64 fd, u64 iov_va, u64 iovcnt_raw) {
    u32 count = 0;
    if (vivarium_writev_decide(iovcnt_raw, &count) != VIV_TRANSLATED)
        return -(s64)T_E_INVAL;

    // A zero count still validates the fd -- Linux resolves the descriptor
    // before it looks at the array, so writev(badfd, x, 0) is EBADF, not 0.
    // Issuing the zero-length write reproduces that through the same core
    // rather than by re-deriving the handle check here.
    if (count == 0) return sys_write_handler(fd, 0, 0);

    // PASS 1 -- read and validate every entry, writing nothing.
    u64 total = 0;
    for (u32 i = 0; i < count; i++) {
        struct viv_linux_iovec kiov;
        u64 ent = iov_va + (u64)i * sizeof(struct viv_linux_iovec);
        // copy_in rather than paired 32-bit loads: it handles an unaligned
        // iov_va (a hostile guest is not obliged to pass an 8-aligned array,
        // and Linux reads one regardless), and a bad VA lands in the fixup.
        if (uaccess_copy_in(&kiov, ent, sizeof(kiov)) != 0) return -(s64)T_E_FAULT;
        if (!vivarium_writev_accumulate(&total, kiov.len)) return -(s64)T_E_INVAL;
        // A zero-length entry names no memory, so it gets no range check --
        // Linux skips them, and sys_validate_user_buf would reject base==0.
        if (kiov.len != 0 && !sys_validate_user_buf(kiov.base, kiov.len))
            return -(s64)T_E_FAULT;
    }

    // PASS 2 -- write, stopping at the first short or failing entry.
    u64 written = 0;
    for (u32 i = 0; i < count; i++) {
        struct viv_linux_iovec kiov;
        u64 ent = iov_va + (u64)i * sizeof(struct viv_linux_iovec);
        if (uaccess_copy_in(&kiov, ent, sizeof(kiov)) != 0)
            return (written > 0) ? (s64)written : -(s64)T_E_FAULT;

        s64 wr = sys_write_handler(fd, kiov.base, kiov.len);

        // POSIX: bytes already transferred WIN over a later error. Reporting the
        // errno instead would tell the guest nothing was written when part of
        // its data is gone -- the write is not undoable, so the count is the
        // only honest answer once it is non-zero.
        if (wr < 0) return (written > 0) ? (s64)written : wr;

        written += (u64)wr;

        // A short entry ends the call. This is also what bounds a single entry
        // larger than SYS_RW_MAX: the core clamps, reports the clamped count,
        // and the guest's libc reissues from where it stopped.
        if ((u64)wr < kiov.len) break;
    }
    return (s64)written;
}

// The TIER-2 shells. Each pairs a PURE translator from kernel/vivarium.c with
// the uaccess + native-core work that translator deliberately refuses to do.
// `ctx` is used by exactly one case (clone, LINEAGE L-3d) and ignored by the
// rest. It is a parameter rather than a second dispatcher because clone IS a
// Tier-2 translation -- a flag map plus an argument reshape -- and splitting it
// out would make VIV_TIER2 mean two different things depending on the number.
//
// Contrast rt_sigreturn, which IS intercepted ahead of the table: that call
// rewrites the frame INSTEAD of returning a value, so the caller's
// `regs[0] = viv_tier2(...)` store would destroy the x0 it just restored.
// Clone is not in that class. It returns a value the normal way, into the
// PARENT's frame -- the child's regs[0] was set to 0 in its own COPY of the
// frame by fork_frame_init, before this function returns, and the child is a
// different Thread on a different stack that never comes back through here.
static s64 viv_tier2(struct exception_context *ctx, struct Proc *p,
                     u64 linux_nr, const u64 *args) {
    switch (linux_nr) {
    case VIV_LINUX_OPENAT: {
        // openat(dirfd, path, flags, mode): x0 dirfd, x1 path, x2 flags.
        u64  start_fd = 0;
        u32  omode    = 0;
        bool cloexec  = false;
        if (vivarium_openat_decide(args[0], args[2], &start_fd, &omode, &cloexec)
                != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;             // out of domain -> V-3 forwards
        // DECIDE BEFORE MEASURE: the measurement is a faultable user read, and
        // a call we were going to hand to the supervisor must not take that
        // fault in the kernel fast path (vivarium.h).
        u32 path_len = 0;
        s64 m = viv_measure_user_path(args[1], &path_len);
        if (m != 0)                                  return m;
        struct viv_call c;
        vivarium_openat_build(start_fd, args[1], path_len, omode, &c);
        s64 fd = sys_open_handler(c.args[0], c.args[1], c.args[2], c.args[3]);
        // #151: O_CLOEXEC is a property of the DESCRIPTOR, so it is applied
        // after the open rather than carried in the omode. Only on success --
        // there is no descriptor to flag otherwise. The set cannot fail here
        // (the fd was just created by this thread and no peer can reach it), so
        // its return is not checked; if it somehow did, the honest report is
        // still the fd, since the file IS open.
        if (fd >= 0 && cloexec)
            (void)handle_set_cloexec(p, (hidx_t)fd, true);
        return fd;
    }

    case VIV_LINUX_FCNTL: {
        // fcntl(fd, cmd, arg): x0 fd, x1 cmd, x2 arg.
        enum viv_fcntl_op op = VIV_FCNTL_UNSERVED;
        bool cloexec = false;
        u64  min_fd  = 0;
        if (vivarium_fcntl_decide(args[1], args[2], &op, &cloexec, &min_fd)
                != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;             // an unserved cmd; see vivarium.h
        // Bound BEFORE narrowing to hidx_t, so a huge or negative-as-int fd
        // cannot wrap into a valid-looking index. The callees range-check too;
        // this exists so the conversion itself is well-defined.
        if (args[0] >= (u64)PROC_HANDLE_MAX)         return -(s64)T_E_BADF;
        hidx_t fd = (hidx_t)args[0];

        switch (op) {
        case VIV_FCNTL_GETFD: {
            int on = handle_get_cloexec(p, fd);
            if (on < 0)                              return -(s64)T_E_BADF;
            return on ? (s64)VIV_FD_CLOEXEC : 0;
        }
        case VIV_FCNTL_SETFD:
            if (handle_set_cloexec(p, fd, cloexec) != 0) return -(s64)T_E_BADF;
            return 0;
        case VIV_FCNTL_DUPFD: {
            // Linux answers EINVAL when the minimum exceeds the fd limit -- an
            // ARGUMENT error, distinct from the EMFILE it gives when the limit
            // is merely reached. The range lives here rather than in the pure
            // decide because PROC_HANDLE_MAX is a fact about the handle table.
            if (min_fd >= (u64)PROC_HANDLE_MAX)      return -(s64)T_E_INVAL;
            hidx_t nfd = handle_dup_posix(p, fd, (hidx_t)min_fd, cloexec);
            // handle_dup_posix folds "no such fd" and "table full" into one -1;
            // split them here, because the two errnos are LOAD-BEARING to a
            // shell and not interchangeable. This used to answer EMFILE for
            // both, on the argument that a guest which just used the fd knows
            // it exists -- but busybox ash's redirect() probes the TARGET fd of
            // every `N>&M` with fcntl(N, F_DUPFD, 10) precisely to learn
            // whether N is open: EBADF means "not open, nothing to save", and
            // ANY other errno is "strange" and aborts the command
            // (`fcntl(3,F_DUPFD,10): No file descriptors available`, once per
            // `3>&1` in the L-6c gate). POSIX: closed fd -> EBADF; table full
            // -> EMFILE. The liveness re-check is the same lookup the GETFD arm
            // uses; the residual -1 set (table full, a non-dup-able kind, a
            // rights failure) stays EMFILE. A peer closing the fd between the
            // dup and this lookup misreports one errno -- unreachable for a
            // single-threaded phenotype Proc, and harmless if it were not.
            if (nfd < 0) {
                if (handle_get_cloexec(p, fd) < 0)  return -(s64)T_E_BADF;
                return -(s64)T_E_MFILE;
            }
            return (s64)nfd;
        }
        default:
            return -(s64)T_E_NOSYS;
        }
    }

    case VIV_LINUX_DUP3: {
        // dup3(old, new, flags): x0 old, x1 new, x2 flags. (#157 -- aarch64 has
        // no dup2 number, so musl compiles dup2() into this; a shell's
        // redirection plumbing cannot reach a pipeline without it.)
        bool cloexec = false;
        if (vivarium_dup3_decide(args[2], &cloexec) != VIV_TRANSLATED)
            return -(s64)T_E_INVAL;   // Linux's OWN answer -- see below

        // LINUX'S CHECK ORDER, REPRODUCED, because it is observable. ksys_dup3
        // does flags-EINVAL, then old==new-EINVAL, then newfd-range-EBADF, then
        // the oldfd lookup. In particular `old == new` is EINVAL *even when old
        // is closed*, because the equality precedes the lookup -- so the two
        // cannot be reordered without changing what a conformance test sees.
        //
        // The EINVAL above is NOT the ENOSYS decline every other T2 row gives
        // for an out-of-domain argument, and the difference is the point: this
        // row's served set is EQUAL to Linux's, not a subset of it, so a flags
        // word we refuse is one Linux refuses too. Answering ENOSYS would claim
        // the surface is absent, which would be false. (V-2d's munmap note made
        // the same distinction for len==0 and a misaligned addr.)
        //
        // Both fds narrow to 32 bits BEFORE the comparison: Linux declares them
        // `unsigned int`, so dup3(1<<32, 0, 0) compares 0 against 0 and is
        // EINVAL there. Comparing the raw registers would call it a valid dup.
        u32 oldfd = (u32)args[0];
        u32 newfd = (u32)args[1];
        if (oldfd == newfd)                return -(s64)T_E_INVAL;
        if (newfd >= (u32)PROC_HANDLE_MAX) return -(s64)T_E_BADF;
        if (oldfd >= (u32)PROC_HANDLE_MAX) return -(s64)T_E_BADF;

        // THE SOCKET DECLINE (vivarium.h carries the three options and why the
        // other two are wrong). It sits HERE -- after every argument error and
        // before any mutation -- so that a decline can never mask an EINVAL or
        // EBADF that Linux would have given for the same call.
        struct viv_socktab *stab =
            __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
        if (viv_socktab_find(stab, (s32)oldfd) != NULL)
            return -(s64)T_E_NOSYS;

        // The install. A -1 here is an empty `old` or a source the alias gate
        // refuses (hardware / Srv / Loom / a devsrv connection Spoor). EBADF is
        // the honest answer to both: Linux has no "this descriptor exists but
        // cannot be duplicated" state, and the second case is unreachable for a
        // phenotyped Proc anyway -- it cannot create such a handle (the create
        // syscalls are native numbers it does not decode) and rfork's copy
        // refuses to inherit one, leaving a hole.
        if (handle_dup_to(p, (hidx_t)oldfd, (hidx_t)newfd, cloexec) < 0)
            return -(s64)T_E_BADF;

        // THE fd-FREEING OBLIGATION, paid for `new`. dup3 closed whatever was
        // there, so a (proto, N) entry keyed on that number must not outlive it.
        //
        // AFTER the install, deliberately, and this is the OPPOSITE of the order
        // viv_sock_accept's unwind uses. That path drops first because it then
        // CLOSES the fd, leaving the number free for the next fd-creating call
        // to be handed while a stale entry still points at it. Here the number
        // is never free -- handle_dup_to overwrites the slot in one lock hold --
        // so no such window exists, and dropping first would instead mean a
        // REFUSED dup3 (the -1 above) had already destroyed the guest's live
        // socket state at `new`. Between the install and this drop nothing of
        // the guest's runs; handle_dup_to's outgoing release may sleep, but a
        // PHENO_LINUX Proc has no peer thread to observe the gap (the property
        // named in struct viv_socktab's header, which must be re-derived if the
        // clone domain ever admits the thread set).
        viv_socktab_drop(stab, (s32)newfd);

        return (s64)newfd;
    }

    case VIV_LINUX_PIPE2: {
        // pipe2(fds, flags): x0 int[2], x1 flags. (#155 -- a shell cannot build
        // a pipeline without it, and aarch64 has no legacy `pipe` to fall back
        // on: musl's pipe() IS this number with flags 0.)
        bool cloexec = false;
        if (vivarium_pipe2_decide(args[1], &cloexec) != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;             // out of domain -> V-3 forwards

        // DECIDE, then RANGE-CHECK, then create. Both refusals precede the
        // allocation on purpose: a call that was never going to land its result
        // should not cost a pipe ring and two descriptors first.
        if (!sys_validate_user_buf(args[0], sizeof(s32) * 2))
            return -(s64)T_E_FAULT;

        hidx_t rd = -1, wr = -1;
        if (sys_pipe_for_proc(p, &rd, &wr) < 0)      return -(s64)T_E_MFILE;

        // THE COPY-OUT IS STILL FALLIBLE despite the range check above, which
        // only proves the VA band -- the page can be absent, read-only, or
        // unmapped by a peer thread between the two. Linux has the identical
        // window (do_pipe2 creates, copies, and closes both on failure), and the
        // cleanup is what makes an EFAULT cost the guest nothing: a returned
        // error with two live descriptors it was never told the numbers of would
        // be an unreachable leak for the life of the Proc.
        s32 pair[2] = { (s32)rd, (s32)wr };
        if (uaccess_copy_out(args[0], pair, sizeof(pair)) != 0) {
            handle_close(p, rd);
            handle_close(p, wr);
            return -(s64)T_E_FAULT;
        }

        // #151, exactly as openat's row does it: close-on-exec is a property of
        // the DESCRIPTOR, so it is applied after creation rather than carried in
        // any flag the pipe itself understands. Both ends, because pipe2's flag
        // is not per-end. The sets cannot fail here (the fds were just made by
        // this thread and no peer can name them yet), and if one somehow did the
        // honest answer is still success -- the pipe IS open.
        if (cloexec) {
            (void)handle_set_cloexec(p, rd, true);
            (void)handle_set_cloexec(p, wr, true);
        }
        return 0;
    }

    case VIV_LINUX_FSTAT: {
        // fstat(fd, statbuf): x0 fd, x1 statbuf.
        struct t_stat ks;
        s64 rc = sys_fstat_for_proc(p, args[0], &ks);
        if (rc != 0)                                 return rc;
        return viv_stat_copy_out(args[1], &ks);
    }

    case VIV_LINUX_NEWFSTATAT: {
        // newfstatat(dirfd, path, statbuf, flags): x0 dirfd, x1 path,
        // x2 statbuf, x3 flags. openat's front half joined to fstat's back
        // half -- which is exactly why vivarium.c has no _fstatat_build.
        // D-1: lstat (AT_SYMLINK_NOFOLLOW) is TRANSLATED now -- the decide's
        // out-param becomes the resolver's no-follow flag.
        u32 viv_sflags = 0;
        if (vivarium_fstatat_decide(args[0], args[3], &viv_sflags)
                != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;
        u32 path_len = 0;
        s64 m = viv_measure_user_path(args[1], &path_len);
        if (m != 0)                                  return m;
        // Copy the path into kernel scratch, exactly as sys_stat_handler does
        // (sys_stat_for_proc's contract is kernel memory, NUL-terminated).
        // The re-read is a SECOND user-memory read, so a peer thread may have
        // rewritten the buffer since the measurement. The kernel copy is
        // well-formed either way (we take exactly path_len bytes and terminate
        // ourselves), and an embedded NUL is rejected exactly as
        // sys_stat_handler rejects it -- sys_stat_for_proc's contract is a
        // NUL-free path, and honouring it here keeps the two callers identical.
        char path_scratch[SYS_OPEN_PATH_MAX + 1];
        for (u32 i = 0; i < path_len; i++) {
            u8 b = 0;
            if (uaccess_load_u8(args[1] + i, &b) != 0) return -(s64)T_E_FAULT;
            if (b == '\0')                             return -(s64)T_E_INVAL;
            path_scratch[i] = (char)b;
        }
        path_scratch[path_len] = '\0';
        struct t_stat ks;
        s64 rc = sys_stat_for_proc(p, path_scratch, path_len,
                                   viv_sflags ? (u32)STALK_NOFOLLOW : 0u, &ks);
        if (rc != 0)                                 return rc;
        return viv_stat_copy_out(args[2], &ks);
    }

    case VIV_LINUX_MMAP: {
        // mmap(addr, len, prot, flags, fd, offset): x0..x5.
        //
        // DISTRO D-3: the FILE arm is tried FIRST, and the order is free rather
        // than load-bearing -- vivarium_mmap_arms_disjoint pins that no tuple is
        // admitted by both, so neither order can shadow the other. Tried first
        // only because it is the narrower domain, which reads better.
        if (vivarium_mmap_file_decide(args[2], args[3], args[4], args[5])
                == VIV_TRANSLATED) {
            // `len` is judged in the shell, not the decide, for the same reason
            // the anon arm judges it here: Linux answers EINVAL for 0 and ENOMEM
            // for too-large, and sys_mmap_file_for_proc reproduces both.
            return sys_mmap_file_for_proc(p, args[4], args[1],
                                          ((u32)args[2] & VIV_PROT_EXEC) != 0,
                                          args[5]);
        }

        // DISTRO D-3b: the two MAP_FIXED overlays. Same disjointness argument --
        // all four arms demand EXACT equality against distinct flags words, so
        // the ordering here cannot shadow anything.
        if (vivarium_mmap_fixed_file_decide(args[0], args[2], args[3],
                                            args[4], args[5]) == VIV_TRANSLATED) {
            return sys_mmap_fixed_file_for_proc(p, args[0], args[4], args[1],
                                                (u32)args[2], args[5]);
        }
        if (vivarium_mmap_fixed_anon_decide(args[0], args[2], args[3],
                                            args[4], args[5]) == VIV_TRANSLATED) {
            return sys_mmap_fixed_anon_for_proc(p, args[0], args[1],
                                                (u32)args[2]);
        }

        if (vivarium_mmap_decide(args[0], args[2], args[3], args[4], args[5])
                != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;             // out of domain

        // `len` is judged HERE, not in the pure decide, so each side's error
        // semantics are reproduced exactly rather than collapsed to ENOSYS:
        // Linux answers EINVAL for 0 ...
        if (args[1] == 0) return -(s64)T_E_INVAL;

        // ... and ENOMEM for a length it cannot satisfy, which is precisely the
        // set sys_burrow_attach_lazy_for_proc refuses (0 / over the lazy cap /
        // no free gap / OOM). Translating its -1 rather than passing it through
        // matters: Thylacine signals failure with a bare -1, and a Linux libc
        // reads -1 as -EPERM.
        s64 rc = sys_burrow_attach_lazy_for_proc(p, args[1]);
        if (rc < 0) return -(s64)T_E_NOMEM;

        // A user VA is below 2^47, so a successful return is never mistaken for
        // an errno -- the [-4095,-1] band a Linux caller checks is unreachable.
        return rc;
    }

    case VIV_LINUX_MUNMAP: {
        // munmap(addr, len): x0 addr, x1 len.
        //
        // #199 widened this row from exact-match to the RANGE form: D-3b's
        // MAP_FIXED split turns one library map into 2-3 VMAs, and musl's
        // unmap_library munmaps the WHOLE span in one call, so exact-match
        // leaked every library torn down on map_library's error path or
        // dlclose. sys_munmap_range_for_proc detaches every VMA WHOLLY inside
        // the range (whole VMAs only -- never partial), treats nothing-mapped
        // as the Linux no-op success, and refuses atomically on a boundary
        // straddle. The range scan it needed (vma_next_overlap_in) now exists,
        // which is what the previous decline-comment said was missing.
        //
        // The two argument errors are reproduced up front because Linux gives
        // them a specific errno that a decline would replace with ENOSYS.
        if (args[0] & (PAGE_SIZE - 1)) return -(s64)T_E_INVAL;
        if (args[1] == 0)              return -(s64)T_E_INVAL;

        if (sys_munmap_range_for_proc(p, args[0], args[1]) == 0) return 0;

        // Outside the served subset: a boundary-straddling partial overlap
        // (Linux would SPLIT the VMA; partial unmap is post-v1.0), a CODE
        // region (I-42's pair lifetime), or out-of-window coordinates.
        // Claiming success would leave a mapping the guest believes is gone.
        // Declining is honest; faking is not.
        return -(s64)T_E_NOSYS;
    }

    case VIV_LINUX_RT_SIGACTION: {
        // rt_sigaction(sig, act, oldact, sigsetsize): x0 sig, x1 act,
        // x2 oldact, x3 sigsetsize.
        //
        // Two user structs, both the fixed 32-byte aarch64 shape (see
        // VIV_KSIGACTION_SIZE for why that is a constant and not a runtime
        // discrimination).
        struct viv_ksigaction act = { .handler = VIV_SIG_DFL, .flags = 0,
                                      .restorer = 0, .mask = 0 };
        if (args[1] != 0) {
            if (!sys_validate_user_buf(args[1], VIV_KSIGACTION_SIZE))
                return -(s64)T_E_FAULT;
            // The whole struct, not just the two fields the decision needs:
            // V-6c honours the handler AND the restorer (the guest's return
            // trampoline), so a partial read would install a handler with no
            // way back.
            if (viv_load_u64(args[1], &act.handler) != 0) return -(s64)T_E_FAULT;
            if (viv_load_u64(args[1] + VIV_KSIGACTION_OFF_FLAGS, &act.flags) != 0)
                return -(s64)T_E_FAULT;
            if (viv_load_u64(args[1] + VIV_KSIGACTION_OFF_RESTORER,
                             &act.restorer) != 0) return -(s64)T_E_FAULT;
            if (viv_load_u64(args[1] + VIV_KSIGACTION_OFF_MASK, &act.mask) != 0)
                return -(s64)T_E_FAULT;
        }

        // A pure QUERY (act == NULL) is decided as a SIG_DFL set: same signal
        // range, same sigsetsize, same "is this a signal we model at all" test.
        // Answering a query about a signal we do not track would mean inventing
        // a disposition for it.
        if (vivarium_sigaction_decide(args[0], act.handler, act.flags, args[3])
                != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;

        enum viv_signote note = viv_signal_note(args[0]);

        // Report the PREVIOUS disposition before installing the new one, so a
        // save/restore pair round-trips. Writing the FULL struct matters --
        // musl reads ksa_old.mask out of an UNINITIALISED stack local, so a
        // short write leaves it holding whatever was on the stack.
        if (args[2] != 0) {
            if (!sys_validate_user_buf(args[2], VIV_KSIGACTION_SIZE))
                return -(s64)T_E_FAULT;
            struct viv_sigtab *cur = __atomic_load_n(&p->sigtab,
                                                     __ATOMIC_ACQUIRE);
            struct viv_ksigaction prev = { .handler = VIV_SIG_DFL, .flags = 0,
                                           .restorer = 0, .mask = 0 };
            if (!viv_sigtab_note_handler(cur, note, &prev) &&
                viv_sigtab_note_ignored(cur, note))
                prev.handler = VIV_SIG_IGN;
            if (viv_store_u64(args[2], prev.handler) != 0)
                return -(s64)T_E_FAULT;
            if (viv_store_u64(args[2] + VIV_KSIGACTION_OFF_FLAGS, prev.flags) != 0)
                return -(s64)T_E_FAULT;
            if (viv_store_u64(args[2] + VIV_KSIGACTION_OFF_RESTORER,
                              prev.restorer) != 0) return -(s64)T_E_FAULT;
            if (viv_store_u64(args[2] + VIV_KSIGACTION_OFF_MASK, prev.mask) != 0)
                return -(s64)T_E_FAULT;
        }

        if (args[1] == 0) return 0;         // query only; nothing to install

        // SIG_DFL is the state a Proc with no table is already in, so it only
        // needs a table when there is one to clear. That keeps a guest which
        // merely RESETS dispositions (the common post-fork cleanup) from
        // allocating anything at all.
        struct viv_sigtab *tab = __atomic_load_n(&p->sigtab, __ATOMIC_ACQUIRE);
        if (!(act.handler == VIV_SIG_DFL && !tab)) {
            if (!tab) {
                tab = viv_sigtab_of(p);
                if (!tab) return -(s64)T_E_NOMEM;
            }
            (void)viv_sigtab_set(tab, note, &act);
        }

        // POSIX 2.4.3 / Linux do_sigaction: a disposition that IGNORES --
        // SIG_IGN, or SIG_DFL for a signal whose default is ignore -- discards
        // every instance already pending, blocked or not. AFTER the store, so
        // notes_post's under-lock disposition read and this discard are one
        // step (see notes_discard_name): a poster that saw SIG_DFL enqueued in
        // a lock hold this one follows, and is removed here; one that takes
        // the lock after this sees the new value and drops. Without it the
        // pending note waited for the EL0 tail's discard arm, and a guest that
        // re-installed a HANDLER before unblocking had it delivered -- a
        // signal POSIX says died at the SIG_IGN. Nothing to discard is the
        // common case and costs one locked scan of an empty ring.
        if (act.handler == VIV_SIG_IGN ||
            (act.handler == VIV_SIG_DFL && viv_signote_default_is_ignore(note))) {
            const char *nm = viv_signote_note_name(note);
            if (nm) (void)notes_discard_name(p, nm);
        }
        return 0;
    }

    case VIV_LINUX_RT_SIGPROCMASK: {
        // rt_sigprocmask(how, set, oldset, sigsetsize): x0 how, x1 set,
        // x2 oldset, x3 sigsetsize.
        //
        // The target is the per-THREAD note_mask, which is the right
        // granularity by construction: notes.h built it "so multi-thread Procs
        // can have different threads accept different signals -- POSIX
        // pthread_sigmask semantics".
        if (vivarium_sigprocmask_decide(args[0], args[3]) != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;

        struct Thread *t = current_thread();
        if (!t) return -(s64)T_E_INVAL;

        u64 want = 0;
        if (args[1] != 0) {
            if (!sys_validate_user_buf(args[1], sizeof(u64)))
                return -(s64)T_E_FAULT;
            if (viv_load_u64(args[1], &want) != 0) return -(s64)T_E_FAULT;
        }

        // Read the old mask BEFORE mutating, and report it out FIRST, so a
        // faulting oldset pointer leaves the mask untouched -- the same
        // observable atomicity sys_note_mask_handler restores by hand on its
        // own writeback failure.
        u64 old_notes = t->note_mask;
        if (args[2] != 0) {
            if (!sys_validate_user_buf(args[2], sizeof(u64)))
                return -(s64)T_E_FAULT;
            if (viv_store_u64(args[2],
                              viv_notemask_to_sigset(old_notes,
                                                     &g_viv_notebits)) != 0)
                return -(s64)T_E_FAULT;
        }

        if (args[1] == 0) return 0;         // query only

        u64 bits = viv_sigset_to_notemask(want, &g_viv_notebits);
        switch (args[0]) {
        case VIV_SIG_BLOCK:   t->note_mask = old_notes | bits;   break;
        case VIV_SIG_UNBLOCK: t->note_mask = old_notes & ~bits;  break;
        // SETMASK replaces outright. Sound here because a Linux-phenotype Proc
        // reaches note_mask through this row only -- SYS_NOTE_MASK is a native
        // number a guest does not call.
        case VIV_SIG_SETMASK: t->note_mask = bits;               break;
        default:              return -(s64)T_E_INVAL;   // decide screened this
        }
        return 0;
    }

    case VIV_LINUX_SOCKET:
        // socket(domain, type, protocol): x0, x1, x2.
        return viv_sock_socket(p, args[0], args[1], args[2]);

    case VIV_LINUX_CONNECT:
        // connect(fd, addr, addrlen): x0 fd, x1 addr, x2 addrlen.
        return viv_sock_connect(p, args[0], args[1], args[2]);

    case VIV_LINUX_BIND:
        // bind(fd, addr, addrlen): x0 fd, x1 addr, x2 addrlen.
        return viv_sock_bind(p, args[0], args[1], args[2]);

    case VIV_LINUX_LISTEN:
        // listen(fd, backlog): x0 fd, x1 backlog.
        return viv_sock_listen(p, args[0], args[1]);

    case VIV_LINUX_ACCEPT:
        // accept(fd, addr, addrlen): x0 fd, x1 addr, x2 addrlen. Linux defines
        // accept as accept4 with no flags, and so does this -- one body, with
        // the flags word pinned to 0 rather than a second near-copy.
        return viv_sock_accept(p, args[0], args[1], args[2], 0);

    case VIV_LINUX_ACCEPT4:
        // accept4(fd, addr, addrlen, flags): x0..x3.
        return viv_sock_accept(p, args[0], args[1], args[2], args[3]);

    case VIV_LINUX_PPOLL:
        // ppoll(fds, nfds, tmo_p, sigmask, sigsetsize): x0..x4. sigsetsize is
        // read only via the sigmask decline, which needs no size -- Linux itself
        // skips that check when the mask is NULL.
        return viv_ppoll(p, args[0], args[1], args[2], args[3]);

    case VIV_LINUX_PSELECT6:
        // pselect6(nfds, rd, wr, ex, tmo, sigmask_and_size): x0..x5 -- the one
        // row that uses all six argument registers, which is why the signal
        // mask arrives packed behind a pointer.
        return viv_pselect6(p, args[0], args[1], args[2], args[3], args[4],
                            args[5]);

    case VIV_LINUX_CLONE: {
        // clone(flags, stack, parent_tid, tls, child_tid): x0..x4, in arm64's
        // CONFIG_CLONE_BACKWARDS order (tls BEFORE child_tid).
        //
        // ONLY args[0] AND args[1] ARE READ, and that is a correctness
        // requirement rather than an economy. posix_spawn calls
        // `__clone(child, stack, flags, arg)` with four arguments, and musl's
        // clone.s then moves x4/x5/x6 into x2/x3/x4 -- three registers the
        // caller never initialised. Linux tolerates that because the
        // corresponding CLONE_* bits are clear; so does this, by refusing every
        // one of those bits in the admitted flags word and passing a LITERAL 0
        // for child_tls. Reaching for args[3] here would hand the child a
        // garbage TPIDR_EL0 and fault it at its first thread-local access, far
        // from this line.
        bool share_mem = false;
        if (vivarium_clone_decide(args[0], args[1], &share_mem) != VIV_TRANSLATED)
            return -(s64)T_E_NOSYS;             // out of domain -> V-3 forwards

        // 0 = INHERIT the caller's TPIDR_EL0. That is what a vfork child needs
        // (it runs the parent's C, thread-locals and all, until it execs) and
        // equally what a fork child needs (it continues the parent outright).
        return sys_rfork_core(ctx,
                              share_mem ? (unsigned)(RFPROC | RFMEM)
                                        : (unsigned)RFPROC,
                              args[1], 0);
    }

    case VIV_LINUX_EXECVE:
        // execve(path, argv, envp): x0..x2. LINEAGE L-6a.
        //
        // Safe as a T2 row, unlike rt_sigreturn, and for a reason worth stating
        // rather than inheriting: sys_execve_core DOES rewrite the frame on
        // success -- but it returns exactly 0, and it has already zeroed every
        // GPR including regs[0]. So the caller's `regs[0] = viv_tier2(...)`
        // store writes 0 onto the 0 it just wrote, and the new image still
        // starts with a clean x0. A core that ever returned non-zero on success
        // would break that, which is why it returns a literal.
        return viv_execve(ctx, args[0], args[1], args[2]);

    case VIV_LINUX_WAIT4:
        // wait4(pid, wstatus, options, rusage): x0..x3. LINEAGE L-6b.
        return viv_wait4(args[0], args[1], args[2], args[3]);

    // ---- the startup batch (#150, LINEAGE L-6c) --------------------------

    case VIV_LINUX_WRITEV:
        // writev(fd, iov, iovcnt): x0..x2.
        return viv_writev(args[0], args[1], args[2]);

    case VIV_LINUX_GETCWD: {
        // getcwd(buf, size): x0 buf, x1 size.
        //
        // A shell rather than a renumber for TWO divergences, either of which
        // alone would disqualify it -- the arguments themselves line up exactly.
        //
        //   1. THE RETURN VALUE IS OFF BY ONE. Linux's raw getcwd returns the
        //      length INCLUDING the terminating NUL (fs/d_path.c); SYS_GETCWD
        //      returns it EXCLUDING. musl happens to survive the difference (it
        //      only tests `ret < 0` and `ret == 0`), but glibc uses the value,
        //      and a length that is quietly one short is precisely the kind of
        //      near-miss that surfaces somewhere unrelated.
        //   2. THE ERROR IS THE WRONG ONE. Linux answers ERANGE for a buffer too
        //      small -- the errno every caller's grow-and-retry loop keys on.
        //      SYS_GETCWD answers a flat -1, which a Linux guest reads as EPERM
        //      (the #100 shape) and no retry loop recognises.
        s64 rc = sys_getcwd_handler(args[0], args[1], 0, 0);
        if (rc < 0) {
            // The native handler folds several causes into -1: a zero size, a
            // buffer too small, an unreadable VA. ERANGE is the one that names
            // a caller ACTION (pass a bigger buffer) and is by far the most
            // likely; EINVAL for the size==0 case Linux distinguishes.
            if (args[1] == 0) return -(s64)T_E_INVAL;
            return -(s64)T_E_RANGE;
        }
        return rc + 1;                      // + the NUL Linux counts
    }

    case VIV_LINUX_GETPPID: {
        // getppid(void). A shell because there IS no native twin -- Thylacine
        // exposes pid/uid/gid (72/73/74) and stops. Adding SYS_GETPPID would be
        // a syscall-interface change, which is an escalation, and it would buy
        // nothing a phenotype-local read does not already give.
        //
        // proc_parent_pid takes g_proc_table_lock, which is not optional and not
        // available here: `parent` is rewritten by proc_reparent_children when a
        // parent exits, so a lockless deref can read a pointer whose Proc is
        // reaped and freed before the field access. The two existing readers
        // (devproc.c, devctl.c) get the lock from proc_for_each; this one cannot,
        // which is why the accessor lives in proc.c beside the tree it walks.
        return (s64)proc_parent_pid(p);
    }

    case VIV_LINUX_GETUID:
        // getuid(void). The native twin is exact, the arity matches, and it is
        // STILL a shell -- the sentinel mapping (vivarium.h) has to happen
        // somewhere, and a T1 renumber has no place to put it.
        return (s64)(u64)vivarium_map_uid(p->principal_id);

    case VIV_LINUX_GETGID:
        // getgid(void). Same shape, same reason.
        return (s64)(u64)vivarium_map_gid(p->primary_gid);

    case VIV_LINUX_UNAME: {
        // uname(buf): x0 buf. A fabrication -- WHAT it claims is the decision,
        // and that argument lives beside the struct in vivarium.h.
        struct viv_linux_utsname uts;
        vivarium_uname_fill(&uts);
        if (!sys_validate_user_buf(args[0], sizeof(uts))) return -(s64)T_E_FAULT;
        if (uaccess_copy_out(args[0], &uts, sizeof(uts)) != 0)
            return -(s64)T_E_FAULT;
        return 0;
    }

    case VIV_LINUX_SET_TID_ADDRESS: {
        // set_tid_address(tidptr): x0. Arity and success semantics match the
        // native handler exactly -- both store the pointer and return the
        // caller's tid -- so this shell exists for ONE reason: the error.
        //
        // Linux's set_tid_address CANNOT fail; it stores whatever pointer it is
        // given. Thylacine validates (4-byte aligned, under the user VA top)
        // because its exit-time clear IS a uaccess_store_u32, which requires the
        // alignment -- so refusing is right and Linux's silent acceptance of a
        // pointer it will never successfully write through is the weaker
        // behaviour. What must not happen is the refusal arriving as a flat -1:
        // musl's __init_tp stores this return value AS THE THREAD'S TID
        // (`td->tid = __syscall(SYS_set_tid_address, ...)`) without checking it,
        // so -1 would silently become the tid. EINVAL is at least an errno the
        // guest can recognise.
        s64 rc = sys_set_tid_address_handler(args[0]);
        return (rc < 0) ? -(s64)T_E_INVAL : rc;
    }

    case VIV_LINUX_SETUID:
        // setuid(uid): x0. Identity is set once at spawn and immutable on a
        // running Proc, so the only call that can be honoured is the no-op --
        // and it MUST be, because setuid(getuid()) is what every "drop to my own
        // uid" path issues and it asks for nothing. Everything else is EPERM,
        // which is both true here and what Linux tells an unprivileged process,
        // so a guest's existing fallback runs unchanged.
        if (args[0] > 0xFFFFFFFFull) return -(s64)T_E_INVAL;
        return vivarium_setid_is_noop((u32)args[0],
                                      vivarium_map_uid(p->principal_id))
                   ? 0 : -(s64)T_E_PERM;

    case VIV_LINUX_SETGID:
        if (args[0] > 0xFFFFFFFFull) return -(s64)T_E_INVAL;
        return vivarium_setid_is_noop((u32)args[0],
                                      vivarium_map_gid(p->primary_gid))
                   ? 0 : -(s64)T_E_PERM;

    default:
        // vivarium_translate said TIER2 for a number with no shell here. That
        // is a table/shell disagreement -- fail closed, never dispatch.
        return -(s64)T_E_NOSYS;
    }
}

// The branch. Returns true when the caller should CONTINUE into the native
// switch (a T1 row, ctx rewritten in place); false when the call is already
// complete and ctx->regs[0] holds its result.
// ---------------------------------------------------------------------------
// The "what does this guest actually need" instrument.
//
// A phenotyped Proc that issues an untranslated number gets -ENOSYS. That is
// the honest ANSWER, but on its own it tells the operator nothing: a stock
// Linux program which dies because it wanted `set_tid_address` and one which
// dies because it wanted `ioctl` are the same silent exit-1. The LINEAGE L-6c
// arc gate is a DETECTOR -- its whole job is to say what is missing -- and a
// detector that reports "the shell exited 1" has not detected anything.
//
// So: name each distinct declined number ONCE, and make the VIVARIUM work list
// mechanical rather than a guessing game (VIVARIUM.md section 4's "the T2
// entries are exactly the FORWARD rows a translator later promotes").
//
// Bounded TWICE, because a diagnostic that a guest can drive is a diagnostic a
// guest can use to flood the console:
//   1. a seen-bitmap makes it exactly-once per number PER PROC;
//   2. a hard report cap covers numbers past the bitmap and any conceivable
//      pathology in the dedupe.
//
// PER PROC, and that word is the whole correctness of this thing. The first
// version deduped SYSTEM-WIDE, which silently destroyed the instrument for
// every consumer after the first: on a boot that runs two containers, the
// second one's declines were all suppressed as "already reported" and its
// census came out EMPTY -- while the first container's census sat in the log
// looking exactly like it belonged to the second. That is worse than no
// diagnostic, because it reads as a measurement. (Found the hard way: an
// Alpine busybox's census turned out to be the viv-pheno-probe's, and the
// wrong reading survived into a written conclusion before the line numbers
// were checked against the surrounding log.)
//
// The bitmap is `fetch_or` and the cap `fetch_add`, so it is SMP-safe without
// a lock; two CPUs racing the same number can at worst both see the pre-set
// bit and print twice, which is a duplicate LOG LINE and nothing else. Two
// phenotyped Procs running CONCURRENTLY will thrash the owner and re-report --
// deliberately the safe direction, since a duplicate line is recoverable and a
// missing one is not, and the report cap still bounds the total.
#define VIV_UNSERVED_BITS         512u   // aarch64 Linux numbers live well under this
#define VIV_UNSERVED_MAX_REPORTS   96u
static u64 g_viv_unserved_seen[VIV_UNSERVED_BITS / 64];
static u32 g_viv_unserved_reports;
static u32 g_viv_unserved_owner;         // whose census the bitmap currently holds

static void viv_report_unserved(u64 nr, const char *why) {
    struct Thread *t   = current_thread();
    u32            pid = (u32)((t && t->proc) ? t->proc->pid : 0);

    // A new Proc starts a fresh census. pid 0 is never a real EL0 Proc, so it
    // cannot collide with a live owner.
    if (__atomic_load_n(&g_viv_unserved_owner, __ATOMIC_RELAXED) != pid) {
        __atomic_store_n(&g_viv_unserved_owner, pid, __ATOMIC_RELAXED);
        for (u32 i = 0; i < VIV_UNSERVED_BITS / 64; i++)
            __atomic_store_n(&g_viv_unserved_seen[i], 0ull, __ATOMIC_RELAXED);
    }

    // Dedupe BEFORE charging the cap: a number asked for in a loop must not
    // burn the budget that a never-yet-seen number needs. ROUND F1: this is a
    // read-only TEST here; the bit is SET below, after the line lands.
    u64 bit = 0ull;
    if (nr < VIV_UNSERVED_BITS) {
        bit = 1ull << (nr & 63u);
        if (__atomic_load_n(&g_viv_unserved_seen[nr >> 6], __ATOMIC_RELAXED) & bit)
            return;
    }
    // ROUND F5: LOAD-then-check, never an unconditional fetch_add. The add ran
    // on every call past the cap too, including the `nr >= VIV_UNSERVED_BITS`
    // arm that skips the dedupe entirely -- so a plain u32 wrapped at 2^32 and
    // re-armed the next 96 lines. Negligible as a channel; reported because
    // the commit body and the status row both assert the cap as a HARD bound
    // for the boot, and asserted bounds get inherited as premises. The
    // load-then-add race can over-print by at most the CPU count, which is
    // strictly better than wrapping.
    if (__atomic_load_n(&g_viv_unserved_reports, __ATOMIC_RELAXED)
        >= VIV_UNSERVED_MAX_REPORTS)
        return;
    // #243: ONE unit under the ring lock, not seven lock-free uart_* calls.
    // The raw loop is the pre-P1-F shape #75 exists to eliminate, and #76
    // removed it from SYS_PUTS one file above after it was observed LIVE
    // shredding a login prompt byte-for-byte against a peer writer. This
    // path arrived later and reached for it again -- and it is the worse
    // site of the two, because an unprivileged EL0 program CHOOSES when it
    // fires by issuing a syscall the phenotype does not serve. A direct
    // uart_puts also bypasses the extinction ring claim, whose hold stops
    // every ring producer and the drain but cannot stop a peer writing the
    // FIFO by another road -- so these bytes could land inside an
    // `EXTINCTION:` line, which costs the multi-boot classifier a real
    // corruption verdict and can invert a test-fault.sh result.
    // ROUND F1 [P1]: the budget is spent on a line that LANDED, never on one
    // that was attempted. `cons_diag_line_emit` is all-or-nothing, so a full
    // ring drops the whole 107-byte unit -- and under back-pressure from a
    // guest writing /dev/cons that drop is DETERMINISTIC, not racy. The first
    // shape of this fix consumed the dedupe bit and the report budget BEFORE
    // the emit, so a dropped line was marked seen forever and the census
    // silently under-reported: exactly the failure this function's own header
    // says the per-Proc rework existed to kill ("worse than no diagnostic,
    // because it reads as a measurement"). The old raw `uart_puts` could not
    // do this -- it span per byte and always emitted.
    //
    // So: emit, and only then take the dedupe bit and charge the cap. A
    // dropped line stays unreported and unspent, and is retried the next time
    // that number is declined.
    struct cons_diag_line l;
    cons_diag_line_init(&l);
    cons_diag_line_puts(&l, "vivarium: unserved linux syscall nr=");
    cons_diag_line_putdec(&l, nr);
    cons_diag_line_puts(&l, " (");
    cons_diag_line_puts(&l, why);
    cons_diag_line_puts(&l, ") pid=");
    cons_diag_line_putdec(&l, (u64)pid);
    cons_diag_line_puts(&l, "\n");
    if (!cons_diag_line_emit(&l))
        return;   // dropped whole: spend nothing, so the next decline retries
    if (bit)
        __atomic_fetch_or(&g_viv_unserved_seen[nr >> 6], bit, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_viv_unserved_reports, 1u, __ATOMIC_RELAXED);
}

// VIV_TRACE: a bounded per-Proc trace of EVERY phenotyped syscall, not just
// the declined ones. Off by default -- this is a bring-up aid for teaching a
// new guest to run, where the question is "what did it do before it died"
// rather than "what did we refuse". The unserved census above is the shipped
// instrument; this one costs a line per syscall and is meant to be switched on
// deliberately, measured, and switched off.
#ifndef VIV_TRACE
#define VIV_TRACE 0
#endif
#if VIV_TRACE
#define VIV_TRACE_PER_PROC 48u
static u32 g_viv_trace_owner;
static u32 g_viv_trace_count;
static void viv_trace_call(u64 nr, struct Proc *p) {
    u32 pid = (u32)(p ? p->pid : 0);
    if (__atomic_load_n(&g_viv_trace_owner, __ATOMIC_RELAXED) != pid) {
        __atomic_store_n(&g_viv_trace_owner, pid, __ATOMIC_RELAXED);
        __atomic_store_n(&g_viv_trace_count, 0u, __ATOMIC_RELAXED);
    }
    if (__atomic_fetch_add(&g_viv_trace_count, 1u, __ATOMIC_RELAXED)
        >= VIV_TRACE_PER_PROC)
        return;
    uart_puts("viv-trace pid=");
    uart_putdec((u64)pid);
    uart_puts(" nr=");
    uart_putdec(nr);
    uart_puts("\n");
}
#endif

// Test support (the burrow_*_for_test convention): drive the T2 fcntl arm on a
// caller-supplied Proc with no exception frame. Deliberately absent from the
// header; the harness extern-declares it. Safe ONLY because the FCNTL case
// reads `p` and `args` and never touches ctx -- a hook that handed a NULL frame
// to the arms that measure user memory (openat, the stat family) would fault,
// so this is not a general T2 driver and must not grow into one.
s64 viv_fcntl_for_test(struct Proc *p, u64 fd, u64 cmd, u64 arg);
s64 viv_fcntl_for_test(struct Proc *p, u64 fd, u64 cmd, u64 arg) {
    u64 args[VIV_NARGS] = { fd, cmd, arg, 0, 0, 0 };
    return viv_tier2(NULL, p, VIV_LINUX_FCNTL, args);
}

static bool viv_linux_dispatch(struct exception_context *ctx, struct Proc *p) {
#if VIV_TRACE
    viv_trace_call(ctx->regs[8], p);
#endif
    // rt_sigreturn is the phenotyped spelling of SYS_NOTED(NCONT) (§6.22), and
    // it is handled HERE rather than by a table row or a viv_tier2 case because
    // it is the one signal call that REWRITES THE EXCEPTION FRAME instead of
    // returning a value. Both of the other shapes are wrong for it:
    //
    //   * a T1 renumber copies the six argument words verbatim, but Linux's
    //     rt_sigreturn takes NO arguments -- x0 holds whatever the handler
    //     returned -- so SYS_NOTED would read a garbage sub-command where it
    //     needs a literal 0.
    //   * viv_tier2 returns an s64 the caller stores into regs[0], which would
    //     immediately overwrite the x0 that notes_noted_restore just restored.
    //
    // It is also why the reject table has no row for 139: a VIV_TIER2 row whose
    // shell does not exist is the "table declares a capability the code lacks"
    // failure viv_tier2's default arm fails closed on. The interception IS the
    // implementation, and the in-guest handler round-trip is its regression --
    // delete this branch and every guest handler runs once and then dies.
    if (ctx->regs[8] == VIV_LINUX_RT_SIGRETURN) {
        sys_noted_handler(ctx, 0);      // NCONT: restore from the Thread snapshot
        return false;                   // ctx is already final; write no result
    }

    u64 args[VIV_NARGS];
    for (u32 i = 0; i < VIV_NARGS; i++) args[i] = ctx->regs[i];

    // V-5: close() releases the socket table entry before the native close
    // runs. This is a HOOK rather than a T2 row on purpose -- close must stay a
    // T1 renumber that falls through to the native handler, so the fd teardown
    // itself keeps exactly one implementation.
    //
    // WITHOUT THIS the fd index is freed while its (proto, N) entry survives,
    // and the next fd-creating syscall gets that index back -- so a later
    // connect() on an unrelated file would find a stale entry and write a dial
    // verb to a STRANGER'S connection. It is the sharpest bug this chunk can
    // have, which is why the drop happens here, unconditionally, before the
    // close can possibly succeed.
    //
    // viv_socktab_drop is a no-op for an fd with no entry (every ordinary
    // file), and the table pointer is read without allocating, so a guest that
    // never made a socket pays one NULL test per close.
    if (ctx->regs[8] == VIV_LINUX_CLOSE) {
        struct viv_socktab *st = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
        if (st) viv_socktab_drop(st, (s32)(s64)ctx->regs[0]);
    }

    struct viv_call call;
    switch (vivarium_translate(ctx->regs[8], args, &call)) {
    case VIV_TRANSLATED:
        ctx->regs[8] = call.nr;
        for (u32 i = 0; i < VIV_NARGS; i++) ctx->regs[i] = call.args[i];
        return true;                            // the NATIVE handler runs

    case VIV_TIER2: {
        s64 t2 = viv_tier2(ctx, p, ctx->regs[8], args);
        // A T2 row that declines is a DIFFERENT fact from a missing row: the
        // translator exists and this ARGUMENT combination fell outside its
        // domain (VIVARIUM.md section 4). Naming them apart is what turns a
        // failing guest into a work list -- "widen this domain" and "write
        // this translator" are different jobs.
        if (t2 == -(s64)T_E_NOSYS)
            viv_report_unserved(ctx->regs[8], "T2 row declined these arguments");
        ctx->regs[0] = (u64)t2;
        return false;
    }

    case VIV_FORWARD:
        // V-3 hands this to the userspace supervisor. Until then the honest
        // answer is the same one ENOSYS gives; this arm exists so that change
        // is one line.
        viv_report_unserved(ctx->regs[8], "no translator (FORWARD)");
        ctx->regs[0] = (u64)(s64)(-(s64)T_E_NOSYS);
        return false;

    case VIV_ENOSYS:
        // A DELIBERATE decline -- the number is in the reject table with a
        // recorded reason (mprotect and I-12, brk and "libc falls to mmap").
        // Still reported, because "we decided against it" is the answer a
        // guest-side failure most needs to hear, and hiding it would make the
        // considered case indistinguishable from the never-considered one.
        viv_report_unserved(ctx->regs[8], "declined by policy");
        ctx->regs[0] = (u64)(s64)(-(s64)T_E_NOSYS);
        return false;

    default:
        viv_report_unserved(ctx->regs[8], "not in the table at all");
        ctx->regs[0] = (u64)(s64)(-(s64)T_E_NOSYS);
        return false;
    }
}

void syscall_dispatch(struct exception_context *ctx) {
    // VIVARIUM V-1b: a phenotyped Proc's numbers are decoded through the
    // translation table before anything else looks at them. A native Proc
    // (phenotype == PHENO_NATIVE, the default and every Proc outside a
    // declared-Linux vivarium) skips this entirely -- one predictable branch
    // on an already-hot cache line, and the native path is byte-unchanged.
    {
        struct Thread *vt = current_thread();
        struct Proc   *vp = vt ? vt->proc : NULL;
        if (vp && vp->phenotype == PHENO_LINUX) {
            if (!viv_linux_dispatch(ctx, vp)) return;
        }
    }

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

    case SYS_EXECVE: {
        // The one handler that must not write its result into regs[0]
        // unconditionally: on SUCCESS it has already zeroed every GPR and
        // repointed elr/sp at the new image, and storing a return value would
        // hand the fresh program a non-zero x0 it never asked for. On FAILURE
        // nothing was touched, so the -errno goes back the normal way.
        s64 r = sys_execve_handler(ctx);
        if (r != 0) ctx->regs[0] = (u64)r;
        return;
    }

    case SYS_RFORK: {
        // The second ctx-taking handler, for the mirror-image reason: execve
        // rewrites this frame so its own eret starts a new image; rfork COPIES
        // it so a second Thread can eret onto it. The child's regs[0] was set
        // to 0 in ITS copy by fork_frame_init and is untouched by this store,
        // which only ever runs on the parent -- the child is a different
        // Thread on a different stack and never returns through here.
        ctx->regs[0] = (u64)sys_rfork_handler(ctx);
        return;
    }

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

    case SYS_PREAD:
        ctx->regs[0] = (u64)sys_pread_handler(ctx->regs[0],
                                              ctx->regs[1],
                                              ctx->regs[2],
                                              ctx->regs[3]);
        return;

    case SYS_PWRITE:
        ctx->regs[0] = (u64)sys_pwrite_handler(ctx->regs[0],
                                               ctx->regs[1],
                                               ctx->regs[2],
                                               ctx->regs[3]);
        return;

    case SYS_YIELD:
        ctx->regs[0] = (u64)sys_yield_handler(ctx->regs[0], ctx->regs[1],
                                              ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_STAT:
        ctx->regs[0] = (u64)sys_stat_handler(ctx->regs[0],   // path_va
                                             ctx->regs[1],   // path_len
                                             ctx->regs[2]);  // stat_va
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
                                                       ctx->regs[3],
                                                       ctx->regs[4]);
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

    case SYS_SETSID:
        ctx->regs[0] = (u64)sys_setsid_handler(ctx->regs[0], ctx->regs[1],
                                               ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_SETPGID:
        ctx->regs[0] = (u64)sys_setpgid_handler(ctx->regs[0], ctx->regs[1],
                                                ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_GETPGID:
        ctx->regs[0] = (u64)sys_getpgid_handler(ctx->regs[0], ctx->regs[1],
                                                ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_GETSID:
        ctx->regs[0] = (u64)sys_getsid_handler(ctx->regs[0], ctx->regs[1],
                                               ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_PTY_REGISTER:
        ctx->regs[0] = (u64)sys_pty_register_handler(ctx->regs[0], ctx->regs[1],
                                                     ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_TTY_SIGNAL:
        ctx->regs[0] = (u64)sys_tty_signal_handler(ctx->regs[0], ctx->regs[1],
                                                   ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_TTY_ACQUIRE:
        ctx->regs[0] = (u64)sys_tty_acquire_handler(ctx->regs[0], ctx->regs[1],
                                                    ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_TTY_SET_FG:
        ctx->regs[0] = (u64)sys_tty_set_fg_handler(ctx->regs[0], ctx->regs[1],
                                                   ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_TTY_GET_FG:
        ctx->regs[0] = (u64)sys_tty_get_fg_handler(ctx->regs[0], ctx->regs[1],
                                                   ctx->regs[2], ctx->regs[3]);
        return;

    case SYS_TTY_CONT:
        ctx->regs[0] = (u64)sys_tty_cont_handler(ctx->regs[0], ctx->regs[1],
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

    case SYS_WEFT_SHARE:
        ctx->regs[0] = (u64)sys_weft_share_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_WEFT_MAP:
        ctx->regs[0] = (u64)sys_weft_map_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_DMA_CREATE_WEAVE:
        ctx->regs[0] = (u64)sys_dma_create_weave_handler(ctx->regs[0],
                                                         ctx->regs[1]);
        return;

    case SYS_WEFT_UNSHARE:
        ctx->regs[0] = (u64)sys_weft_unshare_handler(ctx->regs[0]);
        return;

    case SYS_DMA_CREATE_GPU_BO:
        ctx->regs[0] = (u64)sys_dma_create_gpu_bo_handler(ctx->regs[0],
                                                          ctx->regs[1]);
        return;

    case SYS_BURROW_FROM_HOSTMEM:
        ctx->regs[0] = (u64)sys_burrow_from_hostmem_handler(
            ctx->regs[0], ctx->regs[1], ctx->regs[2], ctx->regs[3],
            ctx->regs[4]);
        return;

    case SYS_HOSTMEM_REFCOUNT:
        ctx->regs[0] = (u64)sys_hostmem_refcount_handler(
            ctx->regs[0], ctx->regs[1]);
        return;

    // I-42 / CL-7k: the JIT capability.
    case SYS_JIT_CREATE:
        ctx->regs[0] = (u64)sys_jit_create_handler(ctx->regs[0], ctx->regs[1]);
        return;

    case SYS_JIT_DESTROY:
        ctx->regs[0] = (u64)sys_jit_destroy_handler(ctx->regs[0]);
        return;

    case SYS_ICACHE_SYNC:
        ctx->regs[0] = (u64)sys_icache_sync_handler(ctx->regs[0], ctx->regs[1]);
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
                                              ctx->regs[4],
                                              ctx->regs[5]);
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

    case SYS_BURROW_ATTACH_LAZY: {
        // CL-4: accept BOTH the native 1-arg ABI (length in x0) AND the Linux
        // 6-arg anon-mmap ABI (addr in x0, length in x1). musl's __init_tls
        // issues a RAW 6-arg mmap for a large-TLS binary such as clang++,
        // bypassing the patched 1-arg __mmap wrapper -- and it is the ONLY raw
        // SYS_mmap site in musl's whole src/ tree (src/env/__init_tls.c).
        //
        // The split is exact rather than heuristic, on three checked facts:
        // that site passes a LITERAL 0 addr under a `tls_size > builtin_tls`
        // guard (so x0==0 and x1>0 always); the patched __mmap rejects both
        // len==0 and MAP_FIXED, so no wrapper call can present a non-zero x0
        // that is really an address; and the native ABI's length is never
        // legally 0 (sys_burrow_attach_lazy_for_proc rejects it).
        //
        // But selecting the 6-arg reading is NOT sufficient. The wrapper's
        // refusals (file-backed, MAP_FIXED) are LIBC-side only -- the kernel
        // sees just x0/x1 and would discard prot/flags/fd/off. A program that
        // calls syscall(SYS_mmap, NULL, len, prot, MAP_PRIVATE, fd, off)
        // directly (public in musl; the whole point of pouch is running ported
        // code that mmaps files) would then receive a valid ANONYMOUS
        // demand-zero mapping where it asked for a FILE -- reading zeros
        // instead of file bytes, with no error. Before CL-4 that same call
        // landed as handler(0) -> -1 -> MAP_FAILED: fail-closed and loud,
        // which is what ARCH 6.5 ("no file-backed mmap by design") requires.
        //
        // So re-check the SHAPE (burrow_lazy_len_from_args), and honour the
        // 6-arg reading only for the exact anonymous-private form the wrapper
        // itself would have allowed. Everything else keeps the pre-CL-4 answer.
        // This also makes a native caller's undefined x1 unreachable in
        // practice, and the native wrappers pin x1 = 0 so that case stays
        // deterministically -1.
        u64 lazy_len = burrow_lazy_len_from_args(ctx->regs[0], ctx->regs[1],
                                                 ctx->regs[3], ctx->regs[4]);
        ctx->regs[0] = (u64)sys_burrow_attach_lazy_handler(lazy_len);
        return;
    }

    case SYS_BURROW_DECOMMIT:
        ctx->regs[0] = (u64)sys_burrow_decommit_handler(ctx->regs[0],
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
