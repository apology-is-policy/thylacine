
#define _POSIX_C_SOURCE 200112L
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_pool_table.h>
typedef struct { int held; } spin_lock_t;
#define ENTRIES_PER_TABLE 512
#define PTE_VALID (1ull << 0)
#define PTE_TYPE_TABLE (1ull << 1)
#define KP_ZERO 1
#define LOOM_MAX_REG_HANDLES 2
#define LOOM_MAX_REG_BUFFERS 2
#define REQUIRE(c, msg) do { if (!(c)) { puts(msg); exit(11); } } while (0)
static int held_locks;
static void spin_lock_init(spin_lock_t *p) { p->held = 0; }
static void spin_lock(spin_lock_t *p) { REQUIRE(!p->held,"recursive lock"); p->held=1; held_locks++; }
static void spin_unlock(spin_lock_t *p) { REQUIRE(p->held,"unlock without lock"); p->held=0; held_locks--; }
static void extinction(const char *s) { puts(s); exit(12); }
struct page { void *mem; bool live; };
static struct page pages[8];
static int nr_pages, page_frees, as_frees, loom_frees, slot_frees, pool_frees, drains, refunds;
static uintptr_t as_addr, loom_addr, slot_addr, pool_addr;
static u64 g_addrspace_next_id, g_loom_destroyed, service_retired_count;
static struct page *alloc_pages(unsigned order, unsigned flags) {
    REQUIRE(order==0 && flags==KP_ZERO && nr_pages<8,"page allocation contract");
    struct page *p=&pages[nr_pages++];
    REQUIRE(!posix_memalign(&p->mem,4096,4096),"host page allocation");
    memset(p->mem,0,4096);p->live=true;return p;
}
static paddr_t page_to_pa(struct page *p) { return (uintptr_t)p->mem; }
static struct page *pa_to_page(paddr_t pa) {
    for(int i=0;i<nr_pages;i++) if((uintptr_t)pages[i].mem==pa) return &pages[i];
    extinction("unknown table page");return NULL;
}
static void *pa_to_kva(paddr_t pa) { return pa_to_page(pa)->mem; }
static void free_pages(struct page *p, unsigned order) {
    REQUIRE(!held_locks && !order && p->live,"table free ownership/lock");
    unsigned char *bytes=p->mem;
    for(int i=0;i<4096;i++) REQUIRE(bytes[i]==0,"table cleared before free");
    p->live=false;page_frees++;free(p->mem);
}
static void *kzalloc(size_t n, unsigned flags) { (void)flags; return calloc(1,n); }
static void kfree(void *p) {
    REQUIRE(!held_locks,"descriptor free outside lock");
    if((uintptr_t)p==as_addr) { REQUIRE(!as_frees,"descriptor freed once"); as_frees++; }
    else if((uintptr_t)p==loom_addr) { REQUIRE(!loom_frees,"Loom freed once"); loom_frees++; }
    else if((uintptr_t)p==slot_addr) { REQUIRE(!slot_frees,"slot page freed once"); slot_frees++; }
    else if((uintptr_t)p==pool_addr) { REQUIRE(!pool_frees,"pool table freed once"); pool_frees++; }
    else extinction("unexpected object free");
    free(p);
}

struct AddrSpace {
    // Total lifetime references, including kernel descriptor pins. Accessed
    // via __atomic_* with ACQ_REL on drop; the final release happens after
    // every holder's final access. owners below separately governs mappings.
    int            ref; // total lifetime: process/constructor owners + kernel pins

    // Was Proc.vma_lock (#713). Serializes every `vmas` mutation and the
    // demand-page reader, and guards the three I-32 counters below so they stay
    // EXACT. Lock order is unchanged by the extraction: as->lock -> burrow
    // v->lock -> buddy zone->lock.
    spin_lock_t    lock;

    // PA of the L0 translation table for TTBR0 (user half). Created by
    // addrspace_alloc, destroyed by the last total lifetime drop.
    paddr_t        pgtable_root;

    // The rolling-ASID context (generation | hardware ASID) -- RW-1 B-F1, ARCH
    // section 6.2.1. 0 == "never assigned" (always misses); stamped at the first
    // context switch by asid_resolve, never freed at teardown (rollover reclaims
    // the whole space at once). Accessed via __atomic_* -- the fast path is
    // lockless.
    u64            context_id;

    // Sorted-by-vaddr_start VMA list: the address-space description against
    // which page faults are dispatched.
    struct Vma    *vmas;

    // I-32 resource axes. All three are charged/uncharged under `lock` so they
    // are exact, and stored with __atomic_store_n so lockless /proc readers get
    // a coherent snapshot.
    u32            page_count;         // the HOLDER count: anon + mapped file pages, and the nodes + tables that map them
    u32            vma_count;          // VMA-slab bound
    u32            shared_map_pages;   // pages shared IN from other Procs (G-2)

    // I-32 shape (A), ratified at the aux-2 merge: the page axis is bounded
    // PER-ADDRESS-SPACE, so the CAP lives beside the COUNT it bounds.
    //
    // Sharing an address space means sharing the pages, so it must also mean
    // sharing the cap. A low-budget Proc that rfork(RFMEM)s into a high-budget
    // address space thereby acquires the high cap -- and that is NOT a
    // privilege escalation, because RFMEM already means the two Procs can write
    // each other's memory. They are mutually trusting by construction; sharing
    // the cap confers no authority that sharing the address space did not.
    //
    // The rejected alternative (count here, cap on the Proc) would let two
    // RFMEM siblings return DIFFERENT verdicts on one shared counter, making
    // the effective bound a property of which sibling happened to fault. A
    // resource bound whose value depends on scheduling is not a bound.
    //
    // Seeded by addrspace_alloc from the creating Proc's AUTHORIZATION
    // (Proc.page_budget). Never 0 on a live address space. Compared against by
    // addrspace_charge_pages -- do NOT reintroduce a caller-supplied cap
    // parameter, which is the rejected shape wearing a different signature.
    u32            page_budget;        // the enforced per-AS cap

    // High-water mark of page_count. Pure telemetry -- no policy reads it.
    // It lives HERE and not on Proc because it mirrors page_count, and a
    // high-water mark separated from the counter it tracks is how the merge
    // nearly shipped a silently-reverted CL-5.
    u32            page_peak;

    // B-1a' audit F1: how much of page_count is user PAGE TABLES (L1/L2/L3,
    // charged by the MMU install, refunded by the reclaim that frees an
    // emptied table). Telemetry -- the cap reads page_count -- so a reader
    // can tell tables from data.
    u32            pgtable_pages;

    // B-1a' audit F8: how much of page_count is FILE pages -- the Image cache's
    // pages this space maps, charged per leaf installed (the holder reading,
    // like a COW-shared page) and refunded per leaf cleared. The page itself
    // is the cache's: it outlives the mapping, and it is what the pool
    // reclaims from an idle image under pressure. Telemetry, like
    // pgtable_pages.
    u32            file_pages;

    // B-1a' audit F4: this space's identity, for the records that outlive it.
    // The eager charge record on a Burrow names the address space that PAID
    // (Burrow.charge_as_id), never the pid: a pid survives exec and would name
    // the successor's space, which never paid. From a global counter at
    // addrspace_alloc; never 0, never reused.
    u64            id;

    // AS-2a: only constructor/Proc references contribute to ownership. A
    // kernel retirement pin keeps the descriptor/account alive, not arbitrary
    // mappings. It cannot execute userspace, share authority, or postpone
    // last-Proc device quiescence. I/O buffers need independent Burrow pins.
    // Separate from ref so those predicates never count cleanup as a mapper.
    // Increment after taking the total ref; decrement before dropping it.
    int            owners;
    // AS-2: live private-ring setup/retirement guards, under lock. Sharing and
    // guard admission are one exclusion domain. Each guard owns a kernel pin;
    // it persists until the final local ring borrow retires, not merely close.
    u32            private_rings;
};

struct Burrow;
struct loom_reg_buffer { struct Burrow *burrow; void *kva; u32 len, _pad; };
struct Loom {
    struct AddrSpace *service_as;
    struct loom_slot_table *service_slots;
    struct loom_service_pool_table *service_pools;
    void *service_fixed;u32 service_fixed_count;
    u32 service_metadata_pages;
    bool sqpoll; u32 n_reg_buf, inflight_ops;
    struct loom_reg_buffer reg_buf[LOOM_MAX_REG_BUFFERS];
    struct { void *spoor; } reg[LOOM_MAX_REG_HANDLES];
    void *ring; u32 magic;
};
static void vma_drain_in(struct AddrSpace *as) {
    REQUIRE(!held_locks && as->owners==0,"last owner drains outside lock");
    drains++;as->vmas=NULL;
}
static bool burrow_unref_settled_in(void *ring, struct AddrSpace *as, u32 *refund) {
    REQUIRE(!held_locks && ring==(void *)1 && as->private_rings==1,"settled ring release");
    REQUIRE(!refunds,"ring released once");refunds++;*refund=3;return true;
}
static void addrspace_uncharge_pages(struct AddrSpace *as, u32 pages) {
    REQUIRE(as->lock.held && as->page_count==pages,"exact retirement refund");
    as->page_count-=pages;
}

static inline void clear_page_then_free(paddr_t pa) {
    u64 *p = (u64 *)pa_to_kva(pa);
    for (u32 i = 0; i < ENTRIES_PER_TABLE; i++) p[i] = 0;
    // F5-style barrier: drain the clearing stores into the inner-
    // shareable domain before the page goes back to the buddy
    // free-list (so the next allocator sees zeroes regardless of CPU).
    __asm__ __volatile__("dsb ish" ::: "memory");
    free_pages(pa_to_page(pa), 0);
}
static void l3_walk_and_free(paddr_t l3_pa) {
    // L3 entries are LEAF page descriptors. We do NOT free the pages
    // they point at — those belong to the VMA layer (BURROW refcount).
    // We zero + free only the L3 table page itself (F4 hardening).
    clear_page_then_free(l3_pa);
}
static void l2_walk_and_free(paddr_t l2_pa) {
    u64 *l2 = (u64 *)pa_to_kva(l2_pa);
    for (u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
        u64 e = l2[i];
        if (!(e & PTE_VALID)) continue;
        // Block descriptor at L2 (2 MiB block) — leaf, no sub-table.
        // Skip the recurse but no free needed (block points at user
        // pages owned by VMA layer, same as L3 leaf pages).
        if (!(e & PTE_TYPE_TABLE)) continue;
        paddr_t l3_pa = e & ~0xFFFull;
        l3_walk_and_free(l3_pa);
    }
    clear_page_then_free(l2_pa);
}
static void l1_walk_and_free(paddr_t l1_pa) {
    u64 *l1 = (u64 *)pa_to_kva(l1_pa);
    for (u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
        u64 e = l1[i];
        if (!(e & PTE_VALID)) continue;
        if (!(e & PTE_TYPE_TABLE)) continue;     // 1 GiB block → leaf
        paddr_t l2_pa = e & ~0xFFFull;
        l2_walk_and_free(l2_pa);
    }
    clear_page_then_free(l1_pa);
}
paddr_t proc_pgtable_create(void) {
    struct page *l0_pg = alloc_pages(0, KP_ZERO);
    if (!l0_pg) return 0;
    return page_to_pa(l0_pg);
}
void proc_pgtable_destroy(paddr_t root) {
    if (root == 0) return;             // kproc / unwound failure path

    u64 *l0 = (u64 *)pa_to_kva(root);
    for (u32 i = 0; i < ENTRIES_PER_TABLE; i++) {
        u64 e = l0[i];
        if (!(e & PTE_VALID)) continue;
        if (!(e & PTE_TYPE_TABLE)) continue;     // unreachable at v1.0 (L0 has no block form)
        paddr_t l1_pa = e & ~0xFFFull;
        l1_walk_and_free(l1_pa);
    }
    // F4 hardening: clear the L0 page contents (table pointers) before
    // it goes back to the buddy. Same rationale as the lower-level
    // walkers — the bytes are PAs of L1 sub-tables we've just freed;
    // letting them leak through a buddy hand-off is a Spectre-class
    // info-leak vector.
    clear_page_then_free(root);
}
struct AddrSpace *addrspace_alloc(u32 page_budget) {
    // I-32 (A): an address space with no cap is the DoS hole the invariant
    // exists to close, so refuse rather than silently treat 0 as unlimited.
    // Every caller has a live Proc whose page_budget is non-zero by
    // construction (proc_alloc seeds it, rfork copies it, the spawn path
    // validates into [1, proc_page_budget_hard_max()]).
    if (page_budget == 0) return NULL;

    struct AddrSpace *as = kzalloc(sizeof(*as), 0);
    if (!as) return NULL;
    as->page_budget = page_budget;
    as->id          = __atomic_add_fetch(&g_addrspace_next_id, 1, __ATOMIC_RELAXED);

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
    __atomic_store_n(&as->owners, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&as->ref, 1, __ATOMIC_RELEASE);
    return as;
}
static void addrspace_lifetime_get(struct AddrSpace *as) {
    if (!as) extinction("addrspace_ref(NULL)");
    int pre = __atomic_fetch_add(&as->ref, 1, __ATOMIC_ACQ_REL);
    // A ref taken on a dead object is a use-after-free in progress; catching it
    // here makes it loud instead of a silently-resurrected address space. Same
    // discipline as the Spoor/SrvConn `pre <= 0` checks.
    if (pre <= 0) extinction("addrspace_ref on a dead AddrSpace");
}
bool addrspace_private_begin(struct AddrSpace *as) {
    if (!as) return false;
    spin_lock(&as->lock);
    if (__atomic_load_n(&as->owners, __ATOMIC_ACQUIRE) != 1 ||
        as->private_rings == ~(u32)0) {
        spin_unlock(&as->lock);
        return false;
    }
    addrspace_lifetime_get(as);
    ++as->private_rings;
    spin_unlock(&as->lock);
    return true;
}
static void addrspace_lifetime_put(struct AddrSpace *as) {
    // NULL-safe: a kernel-only Proc has no address space, and a rollback that
    // fired before addrspace_alloc ran has none either.
    if (!as) return;

    int pre = __atomic_fetch_sub(&as->ref, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0) extinction("addrspace_unref of an already-released AddrSpace");
    if (pre > 1) return;
    if (__atomic_load_n(&as->owners, __ATOMIC_ACQUIRE) != 0)
        extinction("AddrSpace final lifetime drop with live owners");

    // Final descriptor lifetime reference. Ordinary mappings were drained by
    // the final owner in addrspace_unref; kernel pins do not extend mapping
    // ownership. Only local page-table storage remains to destroy here.
    if (as->vmas) extinction("AddrSpace final lifetime drop before mapping drain");
    if (as->private_rings) extinction("AddrSpace final lifetime drop with private rings");

    // B-1a': nothing returns to the pool here. The pool is physical -- every
    // page the drain just freed returned its charge at free_pages, and the
    // tables proc_pgtable_destroy frees below return theirs the same way --
    // and the per-space count dies with the space, as a holder count should.

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
void addrspace_unref(struct AddrSpace *as) {
    if (!as) return;
    int pre = __atomic_fetch_sub(&as->owners, 1, __ATOMIC_ACQ_REL);
    if (pre <= 0) extinction("addrspace_unref without an owner");
    // Keep this owner's lifetime reference across the potentially sleeping FILE
    // clunks. Concurrent kernel unpins cannot free the descriptor during drain.
    // No new owner may be made from an ownerless descriptor; no CPU may still
    // translate through it at this point (the existing exit/exec obligation).
    if (pre == 1) vma_drain_in(as);
    addrspace_lifetime_put(as);
}
void addrspace_private_end(struct AddrSpace *as) {
    if (!as) extinction("addrspace_private_end(NULL)");
    spin_lock(&as->lock);
    if (!as->private_rings) extinction("addrspace_private_end without a guard");
    --as->private_rings;
    spin_unlock(&as->lock);
    addrspace_lifetime_put(as); // may free; never under as->lock
}
bool loom_service_pools_empty(const struct loom_service_pool_table *pools) {
    if (!pools) return false;
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i)
        if (pools->pools[i].phase != LPOOL_EMPTY) return false;
    for (u32 i = 0; i < LOOM_SERVICE_POOL_MEMBERS; ++i)
        if (pools->bank.cells[i].phase != LMEMBER_FREE) return false;
    return true;
}
static void loom_private_drop_buffer(struct AddrSpace *as, struct Burrow *b) {
    if (!b) return;
    u32 refund = 0;
    (void)burrow_unref_settled_in(b, as, &refund);
    if (refund) {
        spin_lock(&as->lock);
        addrspace_uncharge_pages(as, refund);
        spin_unlock(&as->lock);
    }
}
static void loom_private_destroy(struct Loom *l) {
    // Snapshotted before anything frees l: everything after the kfree below
    // reaches the address space through these two locals and never through l.
    struct AddrSpace *as = l->service_as;
    u32 metadata = l->service_metadata_pages;

    // Until the private engine owns its cleanup, refuse every legacy admission
    // path. A retirement must never enter blocking legacy clunk/join code.
    if (l->sqpoll || l->inflight_ops)
        extinction("private Loom reached legacy cleanup state");
    if (l->service_fixed || l->service_fixed_count)
        extinction("private Loom reached cleanup with fixed borrows");
    for (u32 i = 0; i < LOOM_MAX_REG_HANDLES; i++)
        if (l->reg[i].spoor) extinction("private Loom retained a legacy fid");

    // The engine must retire every record before handing its last owner ref to
    // this queue. Today no service handler admits records; fail closed if an
    // internal caller violates that boundary, rather than freeing live borrows.
    if (!l->service_slots) extinction("private Loom lost slot storage");
    for (u32 i = 0; i < LOOM_SERVICE_SLOT_COUNT; ++i)
        if (l->service_slots->slots[i].phase != LSLOT_FREE ||
            l->service_slots->slots[i].pins)
            extinction("private Loom retained a service slot");
    if (!loom_service_pools_empty(l->service_pools))
        extinction("private Loom retained pool storage");
    // No pool, writer or owner borrower survives here. Detach the bounded table
    // before settling each pin outside locks, while the exact image is retained.
    for (u32 i = 0; i < LOOM_MAX_REG_BUFFERS; ++i) {
        struct Burrow *b = l->reg_buf[i].burrow;
        l->reg_buf[i] = (struct loom_reg_buffer){0};
        loom_private_drop_buffer(as, b);
    }
    l->n_reg_buf = 0;

    kfree(l->service_slots);

    // One settled drop rather than claim / drop / restore. The refund is decided
    // inside the same v->lock interval that decides whether this drop ends the
    // ring's occupancy, so a peer holder's final drop cannot land in a gap and
    // leave this one writing a charge record through a freed descriptor. A
    // nonfinal drop reports zero and leaves the record for whichever drop does
    // end the occupancy -- a ring mapping that outlives the fd settles it at VMA
    // teardown, which is why the refund is read from the drop and not from a
    // value claimed before it. The guards above bound LEGACY EXECUTION, BUFFER
    // AND FID state; they say nothing about mapping references, so a second
    // holder of this ring is not merely possible but routine, and no exclusivity
    // argument is available here or relied on.
    u32 refund = 0;
    (void)burrow_unref_settled_in(l->ring, as, &refund);

    l->magic = 0;
    kfree(l);

    // The pin addrspace_private_begin took is what keeps `as` addressable here,
    // so it is released LAST: addrspace_private_end may drop the final lifetime
    // reference and free the descriptor, and nothing may touch `as` after it.
    spin_lock(&as->lock);
    addrspace_uncharge_pages(as, metadata + refund);
    spin_unlock(&as->lock);
    addrspace_private_end(as);

    __atomic_fetch_add(&g_loom_destroyed, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&service_retired_count, 1, __ATOMIC_RELEASE);
}

int main(int argc,char **argv) {
    int order=argc>1?atoi(argv[1]):0;
    struct AddrSpace *as=addrspace_alloc(100);REQUIRE(as,"AddrSpace allocated");
    as_addr=(uintptr_t)as;
    // A root plus L1/L2/L3, with invalid and block entries that are NOT tables.
    u64 *l0=pa_to_kva(as->pgtable_root);
    for(int i=0;i<3;i++) (void)alloc_pages(0,KP_ZERO);
    l0[0]=page_to_pa(&pages[1])|3;l0[1]=0x40000000|PTE_VALID;
    u64 *l1=pages[1].mem,*l2=pages[2].mem,*l3=pages[3].mem;
    l1[0]=page_to_pa(&pages[2])|3;l1[1]=0x80000000|PTE_VALID;
    l2[0]=page_to_pa(&pages[3])|3;l2[1]=0xc0000000|PTE_VALID;
    // A leaf PA must be cleared but never interpreted as a table to free.
    l3[0]=0xdead000|3;
    REQUIRE(addrspace_private_begin(as),"private pin admitted");
    struct Loom *l=calloc(1,sizeof(*l));REQUIRE(l,"Loom allocated");
    loom_addr=(uintptr_t)l;l->service_as=as;l->service_metadata_pages=4;
    l->service_slots=calloc(1,4096);REQUIRE(l->service_slots,"slot page allocated");
    slot_addr=(uintptr_t)l->service_slots;
    l->service_pools=calloc(1,sizeof(*l->service_pools));REQUIRE(l->service_pools,"pool table allocated");
    pool_addr=(uintptr_t)l->service_pools;
    l->ring=(void *)1;as->page_count=7;
    if(order==0) {
        addrspace_unref(as);
        REQUIRE(drains==1 && !as_frees && !page_frees,"private pin retains ownerless image");
        loom_private_destroy(l);
    } else {
        loom_private_destroy(l);
        REQUIRE(!drains && !as_frees && !page_frees,"creator retains retired image");
        REQUIRE(as->ref==1 && !as->private_rings && !as->page_count,"retirement releases only its pin");
        addrspace_unref(as);
    }
    // No descriptor or table pointer is dereferenced beyond final drop.
    REQUIRE(as_frees==1,"final descriptor reclaimed exactly once");
    REQUIRE(page_frees==4,"all four page-table levels reclaimed exactly once");
    REQUIRE(pool_frees==1,"pool table reclaimed exactly once");
    REQUIRE(slot_frees==1,"slot page reclaimed exactly once");
    REQUIRE(loom_frees==1 && refunds==1 && drains==1,"local retirements occur once");
    REQUIRE(service_retired_count==1 && g_loom_destroyed==1,"retirement completion published");
    for(int i=0;i<nr_pages;i++) REQUIRE(!pages[i].live,"no retained table allocation");
    puts("PASS: final descriptor and four table levels freed through extracted code");
    return 0;
}
