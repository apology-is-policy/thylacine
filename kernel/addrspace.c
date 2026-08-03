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
#include <thylacine/burrow.h>    // L-4b: burrow_clone_cow + the type dispatch
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>      // PROC_PAGE_MAX / PROC_VMA_MAX / PROC_SHARED_MAP_MAX_PAGES
#include <thylacine/spinlock.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>       // L-3: the last-reference drain lives here now

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

int addrspace_ref_count(const struct AddrSpace *as) {
    if (!as) return 0;
    return __atomic_load_n(&as->ref, __ATOMIC_ACQUIRE);
}

void addrspace_unref(struct AddrSpace *as) {
    // NULL-safe: a kernel-only Proc has no address space, and a rollback that
    // fired before addrspace_alloc ran has none either.
    if (!as) return;

    int pre = __atomic_fetch_sub(&as->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0) extinction("addrspace_unref of an already-released AddrSpace");
    if (pre > 1) return;

    // Last reference -- so this is the point at which the mappings genuinely
    // stop being anyone's, and therefore the point at which they are freed.
    //
    // L-3 moved the drain here from the callers. It used to run in proc_free
    // (and in proc_exec_replace, on the outgoing space), which was correct only
    // while `ref` could never exceed 1: draining at A DEATH and draining at THE
    // LAST REFERENCE are the same event exactly when there is one reference.
    // Under RFMEM they separate, and the old placement would have had the first
    // sharer to die free a VMA list the survivor was still translating through.
    //
    // Nothing between the decrement and here can take a new reference: a ref is
    // only ever taken from a Proc that already holds one (rfork, from a live
    // parent), so reaching zero means no holder is left to hand one out.
    vma_drain_in(as);

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
// LINEAGE L-4b: the copy-on-write clone (see the header for what each VMA kind
// becomes, and why the parent is modified too).
// =============================================================================

// Does this VMA participate in COW? The two passes below must agree EXACTLY on
// this -- pass 1 clones and flags the child, pass 2 flags the parent and drops its
// writable PTEs -- so the test is written once. A guard VMA has no Burrow and is
// excluded by the first term.
static bool vma_is_cow(const struct Vma *v) {
    return v->burrow && v->burrow->type == BURROW_TYPE_ANON_LAZY;
}

// Build the child's counterpart of one parent VMA. Returns false on any failure;
// the caller then discards the whole partially-built child, so nothing here needs
// to unwind anything except what it allocated and has not yet handed over.
static bool clone_one_vma(struct AddrSpace *dst, bool exempt,
                          const struct Vma *src_vma) {
    // A guard VMA (no Burrow, prot 0) is pure reserved address space. It must be
    // reproduced or the child silently loses its stack guard page -- an overflow
    // would then corrupt the VMA below instead of faulting.
    if (!src_vma->burrow) {
        struct Vma *g = vma_alloc_guard(src_vma->vaddr_start, src_vma->vaddr_end);
        if (!g) return false;
        if (vma_insert_in(dst, exempt, g) != 0) { vma_free(g); return false; }
        return true;
    }

    // Another Proc's memory mapped in here (G-2). Its budget is charged to THIS
    // address space, and a second mapping would need a second charge against a
    // Proc that never asked for it -- refuse rather than invent a policy.
    if (src_vma->flags & VMA_FLAG_SHARED_IN) return false;

    struct Burrow *backing = src_vma->burrow;
    struct Burrow *minted  = NULL;      // a clone we own until the mapping ref lands
    u32            flags   = 0;

    switch (backing->type) {
    case BURROW_TYPE_ANON_LAZY:
        minted = burrow_clone_cow(backing);
        if (!minted) return false;
        backing = minted;
        flags   = VMA_FLAG_COW;
        break;
    case BURROW_TYPE_FILE:
        // Shared, not cloned -- see the header. A writable FILE VMA cannot exist
        // (REVENANT's dispatch gate admits only non-writable segments), so this is
        // a fail-closed check on an impossible shape rather than a live path.
        if (src_vma->prot & VMA_PROT_WRITE) return false;
        break;
    case BURROW_TYPE_ANON:
        // Eager anon is ONE indivisible buddy block, so there is no per-page
        // ownership for a break to take and a WRITABLE one cannot be COW-forked:
        // refuse the fork whole rather than hand the child the wrong sharing
        // semantics. But a READ-ONLY one has nothing to break -- neither Proc can
        // ever write it (there is no prot-mutation syscall; I-12) -- so it is
        // shared outright, exactly as read-only FILE text above is.
        //
        // This is not a corner case, it is EVERY PROC (#136). The vDSO clock page
        // is a single kernel-owned eager-anon page mapped VMA_PROT_READ into every
        // address space by exec_map_vdso, so without this arm addrspace_clone
        // refuses every REAL fork and succeeds only on the synthetic spaces the
        // unit tests build for themselves.
        if (src_vma->prot & VMA_PROT_WRITE) return false;
        break;
    default:
        // MMIO and DMA (I-5 / I-34 hardware). Refused whatever the prot: mapping
        // a device window into a second Proc is an authority transfer, not a
        // memory copy, and read-only does not make it one.
        return false;
    }

    struct Vma *nv = vma_alloc(src_vma->vaddr_start, src_vma->vaddr_end,
                               src_vma->prot, backing, src_vma->burrow_offset);
    if (!nv) {
        if (minted) burrow_unref(minted);
        return false;
    }
    nv->flags = flags;
    if (vma_insert_in(dst, exempt, nv) != 0) {
        vma_free(nv);                   // symmetric: releases the mapping ref
        if (minted) burrow_unref(minted);
        return false;
    }

    if (minted) {
        // The child maps these pages too, so it counts them too (the Linux RSS
        // reading -- see the header on why over-counting here is the safe
        // direction). Read the resident count BEFORE dropping the construction
        // handle, purely so the two operations cannot be reordered by a later
        // edit into a read of a Burrow whose only remaining ref is the mapping.
        u32 resident = burrow_lazy_resident_count(minted);
        burrow_unref(minted);           // handle_count -> 0; the mapping keeps it
        if (resident > 0) {
            spin_lock(&dst->lock);
            bool charged = addrspace_charge_pages(dst, resident, exempt);
            spin_unlock(&dst->lock);
            if (!charged) return false; // over cap -> the fork fails, not the break
        }
    }
    return true;
}

struct AddrSpace *addrspace_clone(struct AddrSpace *src, bool exempt) {
    if (!src) return NULL;

    struct AddrSpace *dst = addrspace_alloc();
    if (!dst) return NULL;

    // src->lock across all three phases: a peer thread of the forking Proc can be
    // in userland_demand_page right now, and it holds exactly this lock.
    spin_lock(&src->lock);

    // PHASE 1 -- drop the parent's own installed PTEs for every COW range, BEFORE
    // anything is shared. They are WRITABLE (the VMA is), so leaving one in place
    // while the child holds a share of the page behind it would let the parent
    // write into the child's memory: the I-44 violation.
    //
    // THE ORDER IS THE WHOLE SAFETY ARGUMENT, and holding src->lock is not a
    // substitute for it (#134). The lock only reaches a peer that FAULTS. A peer
    // holding an already-installed writable PTE stores in hardware -- no fault, no
    // kernel entry, no lock -- so with the uninstall AFTER the snapshot there is a
    // window, lasting the rest of the clone, in which the child already holds a
    // share of a page the parent can still write, silently. Uninstalling first
    // closes it by construction: once the PTE is gone the peer MUST fault, and
    // faulting needs this lock. Zero window, and it is Linux's own structure (it
    // write-protects each PTE under the PTL it copies it under).
    //
    // The flag is deliberately NOT set here -- see phase 3.
    //
    // Uninstalling rather than write-protecting costs one extra fault per page and
    // needs no new MMU primitive. mmu_uninstall_user_range issues the TLBI per
    // page, so a peer already running on another CPU cannot keep using a cached
    // writable translation either. It allocates nothing and cannot fail, which is
    // what lets it run ahead of the decision to commit.
    for (struct Vma *v = src->vmas; v; v = v->next) {
        if (!vma_is_cow(v)) continue;
        mmu_uninstall_user_range(src->pgtable_root, 0,
                                 v->vaddr_start, v->vaddr_end);
    }

    // PHASE 2 -- build the child. The only phase that can fail.
    bool ok = true;
    for (struct Vma *v = src->vmas; v && ok; v = v->next)
        ok = clone_one_vma(dst, exempt, v);

    // PHASE 3 -- flag the parent, on success only, so a FAILED fork leaves it
    // semantically intact: unflagged, so the faults phase 1 just cost it install
    // WRITABLE exactly as before, and it never learns a fork was attempted. (That
    // is why the flag could not join phase 1 despite both touching the parent: the
    // uninstall is recoverable by re-faulting, the flag is not -- VMA_FLAG_COW is
    // never cleared.)
    if (ok) {
        for (struct Vma *v = src->vmas; v; v = v->next)
            if (vma_is_cow(v)) v->flags |= VMA_FLAG_COW;
    }

    spin_unlock(&src->lock);

    if (!ok) {
        // Discard the partial child WHOLESALE: the last unref drains its VMA list,
        // which drops every mapping ref, which frees every Burrow this call
        // cloned, which puts back every COW share it took. Nothing needs a
        // bespoke unwind -- which is why every failure above simply returns false.
        addrspace_unref(dst);
        return NULL;
    }
    return dst;
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
