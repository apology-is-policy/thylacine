// kernel/addrspace.c -- struct AddrSpace lifecycle (LINEAGE L-1).
//
// See <thylacine/addrspace.h> for what lives in the object and why. This file
// is only alloc / ref / unref: the address space's CONTENT (the VMA list, the
// page table's leaves) is managed by vma.c, burrow.c and the fault path, which
// reach it through Proc.as exactly as they used to reach the inline fields.
//
// At L-1 the refcount never exceeds 1. It is written atomically anyway -- see
// the header for why a "1 today" counter is still the wrong place to be lazy.

#include <thylacine/addrspace.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

#include "../arch/arm64/mmu.h"   // proc_pgtable_create / proc_pgtable_destroy
#include "../mm/slub.h"          // kzalloc / kfree

struct AddrSpace *addrspace_alloc(void) {
    struct AddrSpace *as = kzalloc(sizeof(*as), 0);
    if (!as) return NULL;

    // A fresh L0 (KP_ZERO; all 512 entries invalid), installed in TTBR0_EL1 at
    // context switch so each address space's user half is independent. The ASID
    // is NOT assigned here: context_id stays 0 ("never assigned", from kzalloc)
    // and the rolling allocator stamps it at the first context switch
    // (asid_resolve, the context-switch pre-hook; ARCH section 6.2.1). There is
    // no ASID-space exhaustion to roll back from -- rollover recycles the space.
    as->pgtable_root = proc_pgtable_create();
    if (as->pgtable_root == 0) {
        kfree(as);
        return NULL;
    }

    spin_lock_init(&as->lock);
    __atomic_store_n(&as->ref, 1, __ATOMIC_RELEASE);
    return as;
}

void addrspace_ref(struct AddrSpace *as) {
    if (!as) extinction("addrspace_ref(NULL)");
    int pre = __atomic_fetch_add(&as->ref, 1, __ATOMIC_ACQ_REL);
    // A ref taken on a dead object is a use-after-free in progress; catching it
    // here makes it loud instead of a silently-resurrected address space. Same
    // discipline as the Spoor/SrvConn `pre <= 0` checks.
    if (pre <= 0) extinction("addrspace_ref on a dead AddrSpace");
}

void addrspace_unref(struct AddrSpace *as) {
    // NULL-safe: a kernel-only Proc has no address space, and a rollback that
    // fired before addrspace_alloc ran has none either.
    if (!as) return;

    int pre = __atomic_fetch_sub(&as->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0) extinction("addrspace_unref of an already-released AddrSpace");
    if (pre > 1) return;

    // Last reference. The VMA list must already be drained -- proc_free runs
    // vma_drain well before it reaches here -- so the only thing left to
    // release is the page table itself.
    if (as->vmas) extinction("addrspace free with a live VMA list (caller must drain)");

    // No TLB flush here, and no per-address-space ASID free: the rolling-ASID
    // model simply drops context_id, and its hardware ASID value stays reserved
    // in the current generation's bitmap until the next rollover reclaims the
    // whole space at once (ARCH section 6.2.1). This is TLB-safe because the
    // leaf user mappings were already invalidated by vma_drain's all-ASID
    // `tlbi vaae1is`, no live CPU holds this table in TTBR0 (every thread was
    // reaped and on_cpu-spun first), and any eventual reuse of the ASID value
    // is gated by the rollover's per-CPU flush_pending local flush. Matches the
    // Linux model: no flush at teardown, reclaim at rollover.
    proc_pgtable_destroy(as->pgtable_root);
    as->pgtable_root = 0;
    kfree(as);
}
