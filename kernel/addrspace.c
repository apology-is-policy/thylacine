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
#include <thylacine/proc.h>      // PROC_PAGE_MAX / PROC_VMA_MAX / PROC_SHARED_MAP_MAX_PAGES
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
    // whole space at once (ARCH section 6.2.1). What makes that safe is the
    // ASID TAG -- every user PTE is non-global (PTE_NG), so a stale entry is
    // reachable only under this address space's own ASID, and the rollover's
    // per-CPU flush_pending local flush runs before that value can go live
    // again. Each caller separately owes "no CPU translates under this ASID
    // now": proc_free by having reaped + on_cpu-spun every thread,
    // proc_exec_replace by writing the new TTBR0 (a DIFFERENT ASID) and `isb`ing
    // before it gets here. See the header for the full argument -- in
    // particular, vma_drain issues NO TLBI (measured at L-2), so an earlier
    // claim that it did was fiction and is not load-bearing anywhere.
    proc_pgtable_destroy(as->pgtable_root);
    as->pgtable_root = 0;
    kfree(as);
}

// =============================================================================
// The I-32 counter mechanics (see the header for the policy/mechanics split).
// =============================================================================
//
// Every one of these runs under as->lock, which is what makes the caps EXACT:
// the load, the cap decision and the store cannot interleave with a sibling
// charge on the same address space. The uncharges clamp at 0 rather than
// wrapping -- every uncharge pairs with a charge, so a wrap would mean the
// pairing is already broken and silently producing a 4-billion-page counter
// would hide it.

bool addrspace_charge_pages(struct AddrSpace *as, u32 npages, bool exempt) {
    if (!as) return false;
    u32 cur = __atomic_load_n(&as->page_count, __ATOMIC_RELAXED);
    if (npages > 0xFFFFFFFFu - cur) return false;    // counter overflow (refuse)
    if (!exempt && cur + npages > PROC_PAGE_MAX)
        return false;                                 // over cap -> caller -ENOMEM
    __atomic_store_n(&as->page_count, cur + npages, __ATOMIC_RELEASE);
    return true;
}

void addrspace_uncharge_pages(struct AddrSpace *as, u32 npages) {
    if (!as) return;
    u32 cur = __atomic_load_n(&as->page_count, __ATOMIC_RELAXED);
    u32 nv  = (cur >= npages) ? cur - npages : 0;
    __atomic_store_n(&as->page_count, nv, __ATOMIC_RELEASE);
}

bool addrspace_charge_vma(struct AddrSpace *as, bool exempt) {
    if (!as) return false;
    u32 cur = __atomic_load_n(&as->vma_count, __ATOMIC_RELAXED);
    if (cur == 0xFFFFFFFFu) return false;             // counter saturation (refuse)
    if (!exempt && cur >= PROC_VMA_MAX)
        return false;                                 // over cap -> vma_insert rejects
    __atomic_store_n(&as->vma_count, cur + 1, __ATOMIC_RELEASE);
    return true;
}

void addrspace_uncharge_vma(struct AddrSpace *as) {
    if (!as) return;
    u32 cur = __atomic_load_n(&as->vma_count, __ATOMIC_RELAXED);
    u32 nv  = (cur > 0) ? cur - 1 : 0;
    __atomic_store_n(&as->vma_count, nv, __ATOMIC_RELEASE);
}

bool addrspace_charge_shared_map(struct AddrSpace *as, u32 npages, bool exempt) {
    if (!as) return false;
    u32 cur = __atomic_load_n(&as->shared_map_pages, __ATOMIC_RELAXED);
    if (npages > 0xFFFFFFFFu - cur) return false;    // counter overflow (refuse)
    if (!exempt && cur + npages > PROC_SHARED_MAP_MAX_PAGES)
        return false;                                 // over cap -> the share fails clean
    __atomic_store_n(&as->shared_map_pages, cur + npages, __ATOMIC_RELEASE);
    return true;
}

void addrspace_uncharge_shared_map(struct AddrSpace *as, u32 npages) {
    if (!as) return;
    u32 cur = __atomic_load_n(&as->shared_map_pages, __ATOMIC_RELAXED);
    u32 nv  = (cur >= npages) ? cur - npages : 0;
    __atomic_store_n(&as->shared_map_pages, nv, __ATOMIC_RELEASE);
}
