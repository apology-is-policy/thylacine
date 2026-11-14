// Shared anonymous memory (operator votes 117 + 119; ARCH 28 I-44 + I-32;
// VIVARIUM 6.21; specs/payer.tla): the BURROW_TYPE_ANON_SHARED Burrow a fork
// shares, and the payer record that pays for its pages.
//
// Each witness drives the real paths -- the phenotype's mint core, the fault
// arm (userland_demand_page), addrspace_clone, the munmap and madvise cores --
// and reads the result where its consumer does: the leaf PTEs both spaces
// install, the space's page count, the record. Every refund-to-baseline
// witness takes a floor first (a fresh space's data count is 0, and a refund
// past 0 clamps there, so a baseline of 0 cannot show an over-refund).

#include "test.h"

#include "../../arch/arm64/fault.h"
#include "../../mm/phys.h"

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/syscall.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#define SA_PAGES   4u
#define SA_FLOOR   4u
#define SA_LEN     ((u64)SA_PAGES * PAGE_SIZE)
#define SA_PATTERN 0x5A5AC0DEF00D1234ull

// AP[2:1] at PTE bits 7:6 -- 0b01 user-RW.
#define SA_AP_FIELD  (3ull << 6)
#define SA_AP_RW_ANY (1ull << 6)

static void sa_drop(struct Proc *p) {
    if (!p) return;
    p->state = PROC_STATE_ZOMBIE;
    proc_free(p);
}

static u32 sa_data(struct AddrSpace *as) { return as->page_count - as->pgtable_pages; }
static bool sa_floor(struct AddrSpace *as) { return addrspace_charge_pages(as, SA_FLOOR, false); }
static void sa_unfloor(struct AddrSpace *as, bool floored) {
    if (floored) addrspace_uncharge_pages(as, SA_FLOOR);
}

// The phenotype's anonymous mint (sys_mmap_anon_core) at RW under an RW
// ceiling; 0 on any refusal.
static u64 sa_map(struct Proc *p, bool shared) {
    s64 va = sys_mmap_anon_core(p, SA_LEN, VMA_PROT_RW, VMA_PROT_RW, shared);
    return va > 0 ? (u64)va : 0;
}

static enum fault_result sa_fault(struct Proc *p, u64 va, bool write) {
    struct fault_info fi;
    fi.vaddr          = va;
    fi.elr            = 0;
    fi.esr            = 0;
    fi.ec             = 0x24;            // EC_DATA_ABORT_LOWER
    fi.fsc            = 0x07;            // FSC_TRANS_FAULT_L3
    fi.fault_level    = 3;
    fi.from_user      = true;
    fi.is_instruction = false;
    fi.is_write       = write;
    fi.is_translation = true;
    fi.is_permission  = false;
    fi.is_access_flag = false;
    fi.is_alignment   = false;
    fi.is_external    = false;
    return userland_demand_page(p, &fi);
}

// The valid L3 leaf covering `va` in the tree at `root`, or 0.
static u64 sa_leaf(paddr_t root, u64 va) {
    const u64 VALID = 1ull << 0, TABLE = 1ull << 1;
    u64 *t = (u64 *)pa_to_kva(root);
    for (int lvl = 0; lvl < 3; lvl++) {
        u64 e = t[(va >> (39 - 9 * lvl)) & 0x1ff];
        if (!(e & VALID) || !(e & TABLE)) return 0;
        t = (u64 *)pa_to_kva(e & 0x0000FFFFFFFFF000ull);
    }
    u64 leaf = t[(va >> 12) & 0x1ff];
    return (leaf & VALID) ? leaf : 0;
}
static paddr_t sa_leaf_pa(u64 leaf) { return (paddr_t)(leaf & 0x0000FFFFFFFFF000ull); }

// A fork's child space, run through `q` (whose own space is parked, then put back).
static struct AddrSpace *sa_fork(struct Proc *p, struct Proc *q, struct AddrSpace **qown) {
    struct AddrSpace *c = addrspace_clone(p->as, false);
    *qown = q->as;
    if (c) q->as = c;
    return c;
}
static void sa_unfork(struct Proc *q, struct AddrSpace *qown, struct AddrSpace *c) {
    q->as = qown;
    if (c) addrspace_unref(c);
}

static struct Burrow *sa_burrow(struct Proc *p, u64 va) {
    struct Vma *v = vma_lookup(p, va);
    return v ? v->burrow : NULL;
}

// The fork shares the Burrow itself: the child's write lands in the page the
// parent maps (one physical page behind both leaves), and the parent's
// writable leaf survives the fork. The private map beside it is the control,
// one variable away: the same fork routes it through COW, and the child's
// write gets a copy.
void test_shared_anon_fork_shares_the_page(void) {
    struct Proc *p = proc_alloc(), *q = proc_alloc();
    if (!p || !q) { sa_drop(q); sa_drop(p); TEST_ASSERT(false, "proc_alloc"); }
    u64 sva = sa_map(p, true), pva = sa_map(p, false);
    enum fault_result rs0 = sva ? sa_fault(p, sva, true) : FAULT_UNHANDLED_USER;
    enum fault_result rp0 = pva ? sa_fault(p, pva, true) : FAULT_UNHANDLED_USER;
    struct Burrow *sb = sva ? sa_burrow(p, sva) : NULL;
    bool minted_shared = sb && sb->type == BURROW_TYPE_ANON_SHARED;
    paddr_t ps0 = sva ? sa_leaf_pa(sa_leaf(p->as->pgtable_root, sva)) : 0;
    paddr_t pp0 = pva ? sa_leaf_pa(sa_leaf(p->as->pgtable_root, pva)) : 0;

    struct AddrSpace *qown = NULL;
    struct AddrSpace *c = (sva && pva) ? sa_fork(p, q, &qown) : NULL;
    bool same = false, child_plain = false, parent_payer = false, parent_rw = false;
    paddr_t ps = 0, cs = 0, cp = 0;
    enum fault_result rs1 = FAULT_UNHANDLED_USER, rp1 = FAULT_UNHANDLED_USER;
    u32 resident = 0;
    if (c) {
        struct Vma *cv = vma_lookup(q, sva);
        struct Vma *sv = vma_lookup(p, sva);
        same         = cv && cv->burrow == sb;
        child_plain  = cv && !(cv->flags & (VMA_FLAG_COW | VMA_FLAG_PAYER)) &&
                       vma_prot_max(cv) == VMA_PROT_RW;
        parent_payer = sv && !(sv->flags & VMA_FLAG_COW) && (sv->flags & VMA_FLAG_PAYER);
        u64 leaf     = sa_leaf(p->as->pgtable_root, sva);
        parent_rw    = leaf && (leaf & SA_AP_FIELD) == SA_AP_RW_ANY;
        rs1 = sa_fault(q, sva, true);
        rp1 = sa_fault(q, pva, true);
        ps  = sa_leaf_pa(sa_leaf(p->as->pgtable_root, sva));
        cs  = sa_leaf_pa(sa_leaf(c->pgtable_root, sva));
        cp  = sa_leaf_pa(sa_leaf(c->pgtable_root, pva));
        resident = burrow_lazy_resident_count(sb);
    }
    sa_unfork(q, qown, c);
    sa_drop(q);
    sa_drop(p);

    TEST_ASSERT(sva != 0 && pva != 0, "the shared and the private mint");
    TEST_ASSERT(minted_shared, "MAP_SHARED|MAP_ANONYMOUS mints an ANON_SHARED Burrow");
    TEST_EXPECT_EQ(rs0, FAULT_HANDLED, "the parent's write faults in the shared page");
    TEST_EXPECT_EQ(rp0, FAULT_HANDLED, "and the private one");
    TEST_ASSERT(ps0 != 0 && pp0 != 0, "both leaves installed before the fork");
    TEST_ASSERT(c != NULL, "the fork");
    TEST_ASSERT(same, "the child maps the parent's Burrow, not a clone");
    TEST_ASSERT(child_plain, "the child's mapping: no COW routing, no payer flag, the RW ceiling");
    TEST_ASSERT(parent_payer, "the parent's mapping stays the payer's, never COW");
    TEST_ASSERT(parent_rw, "the fork left the parent's writable leaf in place");
    TEST_EXPECT_EQ(rs1, FAULT_HANDLED, "the child's write to the shared page");
    TEST_EXPECT_EQ(rp1, FAULT_HANDLED, "and to the private one");
    TEST_EXPECT_EQ((u64)cs, (u64)ps0, "the child's write lands in the parent's page");
    TEST_EXPECT_EQ((u64)ps, (u64)ps0, "and the parent still maps it");
    TEST_EXPECT_EQ(resident, 1u, "one page behind both leaves: nothing was copied");
    TEST_ASSERT(cp != 0 && cp != pp0, "control: the private page's child write got a copy");
}

// The creator pays every mint, whoever faults: the child's mint is charged
// to the parent and the child pays only its leaf; the parent's own leaves
// cost it nothing past the mint.
void test_shared_anon_creator_pays_every_mint(void) {
    struct Proc *p = proc_alloc(), *q = proc_alloc();
    if (!p || !q) { sa_drop(q); sa_drop(p); TEST_ASSERT(false, "proc_alloc"); }
    u64 sva = sa_map(p, true);
    struct Burrow *sb = sva ? sa_burrow(p, sva) : NULL;
    const bool pfl = sa_floor(p->as);
    struct AddrSpace *qown = NULL;
    struct AddrSpace *c = sva ? sa_fork(p, q, &qown) : NULL;
    bool cfl = false, payer_p = false;
    u32 d0 = 0, c0 = 0, dp1 = 0, dc1 = 0, rec1 = 0, dp2 = 0, rec2 = 0, dp3 = 0, dc3 = 0, rec3 = 0;
    enum fault_result r1 = FAULT_UNHANDLED_USER, r2 = FAULT_UNHANDLED_USER,
                      r3 = FAULT_UNHANDLED_USER;
    if (c) {
        cfl = sa_floor(c);
        d0 = sa_data(p->as); c0 = sa_data(c);
        r1 = sa_fault(q, sva, true);                    // the child mints slot 0
        dp1 = sa_data(p->as); dc1 = sa_data(c);
        rec1 = burrow_payer_charge_pages_for_test(sb);
        payer_p = burrow_payer_for_test(sb) == p->as;
        r2 = sa_fault(p, sva, false);                   // the parent's hit: a payer leaf
        dp2 = sa_data(p->as);
        rec2 = burrow_payer_charge_pages_for_test(sb);
        r3 = sa_fault(p, sva + PAGE_SIZE, true);        // the parent mints slot 1
        dp3 = sa_data(p->as); dc3 = sa_data(c);
        rec3 = burrow_payer_charge_pages_for_test(sb);
        sa_unfloor(c, cfl);
    }
    sa_unfork(q, qown, c);
    sa_unfloor(p->as, pfl);
    sa_drop(q);
    sa_drop(p);

    TEST_ASSERT(sva != 0, "the shared mint");
    TEST_ASSERT(pfl && cfl, "the floors under both counts");
    TEST_ASSERT(c != NULL, "the fork");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "the child's mint is served");
    TEST_ASSERT(payer_p, "the creator is the payer");
    TEST_EXPECT_EQ(dp1, d0 + 1u, "the creator paid the child's mint");
    TEST_EXPECT_EQ(dc1, c0 + 1u, "the child paid its one leaf");
    TEST_EXPECT_EQ(rec1, 1u, "the record holds the page");
    TEST_EXPECT_EQ(r2, FAULT_HANDLED, "the creator's hit");
    TEST_EXPECT_EQ(dp2, dp1, "a payer leaf costs nothing more");
    TEST_EXPECT_EQ(rec2, 1u, "a hit mints nothing");
    TEST_EXPECT_EQ(r3, FAULT_HANDLED, "the creator's mint");
    TEST_EXPECT_EQ(dp3, dp2 + 1u, "the creator paid its own mint, and no leaf");
    TEST_EXPECT_EQ(dc3, dc1, "the child paid nothing for it");
    TEST_EXPECT_EQ(rec3, 2u, "the record is the resident count");
}

// An unmap never releases a shared page (I-7): the payer unmaps every mapping
// it has and the page stays, contents and all, behind the child's leaf --
// still paid by the payer, which only its death lapses. The last mapping's
// unmap frees the Burrow, refunding the record whole to the live payer and the
// child's leaf to the child.
void test_shared_anon_unmap_keeps_the_page(void) {
    struct Proc *p = proc_alloc(), *q = proc_alloc();
    if (!p || !q) { sa_drop(q); sa_drop(p); TEST_ASSERT(false, "proc_alloc"); }
    u64 sva = sa_map(p, true);
    struct Burrow *sb = sva ? sa_burrow(p, sva) : NULL;
    const bool pfl = sa_floor(p->as);
    const u32 d0 = sa_data(p->as);
    enum fault_result r0 = sva ? sa_fault(p, sva, true) : FAULT_UNHANDLED_USER;
    struct page *pg = sb ? burrow_lazy_slot_for_test(sb, 0) : NULL;
    if (pg) *(volatile u64 *)pa_to_kva(page_to_pa(pg)) = SA_PATTERN;
    struct AddrSpace *qown = NULL;
    struct AddrSpace *c = pg ? sa_fork(p, q, &qown) : NULL;
    bool cfl = false, still_payer = false, kept = false;
    u32 c0 = 0, dc1 = 0, dp1 = 0, rec1 = 0, res1 = 0, dp2 = 0, dc2 = 0;
    s64 m1 = -1, m2 = -1;
    enum fault_result r1 = FAULT_UNHANDLED_USER;
    if (c) {
        cfl = sa_floor(c);
        c0 = sa_data(c);
        r1 = sa_fault(q, sva, false);                   // the child's hit: a charged leaf
        dc1 = sa_data(c);
        m1 = sys_munmap_range_for_proc(p, sva, SA_LEN); // the payer unmaps everything
        still_payer = burrow_payer_for_test(sb) == p->as;
        rec1 = burrow_payer_charge_pages_for_test(sb);
        res1 = burrow_lazy_resident_count(sb);
        dp1 = sa_data(p->as);
        paddr_t cpa = sa_leaf_pa(sa_leaf(c->pgtable_root, sva));
        kept = cpa == page_to_pa(pg) &&
               *(volatile u64 *)pa_to_kva(cpa) == SA_PATTERN;
        m2 = sys_munmap_range_for_proc(q, sva, SA_LEN); // the last mapping: the free
        dp2 = sa_data(p->as); dc2 = sa_data(c);
        sa_unfloor(c, cfl);
    }
    sa_unfork(q, qown, c);
    sa_unfloor(p->as, pfl);
    sa_drop(q);
    sa_drop(p);

    TEST_ASSERT(sva != 0 && pg != NULL, "the shared mint and its page");
    TEST_ASSERT(pfl && cfl, "the floors under both counts");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "the payer mints");
    TEST_ASSERT(c != NULL, "the fork");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "the child's hit");
    TEST_EXPECT_EQ(dc1, c0 + 1u, "the child paid its leaf");
    TEST_EXPECT_EQ(m1, (s64)0, "the payer's munmap");
    TEST_ASSERT(still_payer, "an unmap does not lapse the record: only death does");
    TEST_EXPECT_EQ(rec1, 1u, "the record keeps the page");
    TEST_EXPECT_EQ(res1, 1u, "the page stays resident: an unmap never releases it");
    TEST_EXPECT_EQ(dp1, d0 + 1u, "and the payer still pays for it");
    TEST_ASSERT(kept, "the child's leaf still names the page, the contents intact");
    TEST_EXPECT_EQ(m2, (s64)0, "the child's munmap, the last mapping");
    TEST_EXPECT_EQ(dp2, d0, "the free refunded the record whole to the live payer");
    TEST_EXPECT_EQ(dc2, c0, "and the child's leaf to the child");
}

// The payer's death lapses the record; the next sharer to mint becomes the
// payer, charged the whole residency plus its mint, and its mapping becomes a
// payer mapping (its charged leaf uninstalled and refunded, then re-faulted
// free).
void test_shared_anon_creator_death_hands_over(void) {
    struct Proc *p = proc_alloc(), *q = proc_alloc();
    if (!p || !q) { sa_drop(q); sa_drop(p); TEST_ASSERT(false, "proc_alloc"); }
    u64 sva = sa_map(p, true);
    enum fault_result r0 = sva ? sa_fault(p, sva, true) : FAULT_UNHANDLED_USER;
    struct AddrSpace *qown = NULL;
    struct AddrSpace *c = sva ? sa_fork(p, q, &qown) : NULL;
    bool cfl = false, lapsed = false, handed = false, flagged = false, old_leaf_gone = false;
    u32 c0 = 0, dc1 = 0, rec_l = 0, rec2 = 0, dc2 = 0, dc3 = 0;
    enum fault_result r1 = FAULT_UNHANDLED_USER, r2 = FAULT_UNHANDLED_USER,
                      r3 = FAULT_UNHANDLED_USER;
    if (c) {
        cfl = sa_floor(c);
        c0 = sa_data(c);
        r1 = sa_fault(q, sva, false);                   // a charged leaf
        dc1 = sa_data(c);
        struct Vma *cv = vma_lookup(q, sva);
        struct Burrow *cb = cv ? cv->burrow : NULL;     // alive in the child's mapping
        sa_drop(p);                                     // the payer dies
        p = NULL;
        lapsed = cb && burrow_payer_for_test(cb) == NULL;
        rec_l  = cb ? burrow_payer_charge_pages_for_test(cb) : 0;
        r2 = sa_fault(q, sva + PAGE_SIZE, true);        // a mint into the lapsed record
        handed  = cb && burrow_payer_for_test(cb) == c;
        rec2    = cb ? burrow_payer_charge_pages_for_test(cb) : 0;
        flagged = cv && (cv->flags & VMA_FLAG_PAYER);
        old_leaf_gone = sa_leaf(c->pgtable_root, sva) == 0;
        dc2 = sa_data(c);
        r3 = sa_fault(q, sva, false);                   // re-faults as a payer leaf
        dc3 = sa_data(c);
        sa_unfloor(c, cfl);
    }
    sa_unfork(q, qown, c);
    sa_drop(q);
    sa_drop(p);

    TEST_ASSERT(sva != 0, "the shared mint");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "the creator mints");
    TEST_ASSERT(c != NULL && cfl, "the fork, and the floor under the child");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "the child's hit");
    TEST_EXPECT_EQ(dc1, c0 + 1u, "the child paid that leaf");
    TEST_ASSERT(lapsed, "the creator's death lapses the record");
    TEST_EXPECT_EQ(rec_l, 1u, "the lapsed record keeps the resident count");
    TEST_EXPECT_EQ(r2, FAULT_HANDLED, "the child's mint is served");
    TEST_ASSERT(handed, "the minting sharer became the payer");
    TEST_EXPECT_EQ(rec2, 2u, "the record: 1 inherited + 1 minted");
    TEST_ASSERT(flagged, "the new payer's mapping became a payer mapping");
    TEST_ASSERT(old_leaf_gone, "the adoption uninstalled the leaf it had paid for");
    TEST_EXPECT_EQ(dc2, c0 + 2u, "the new payer pays the whole residency, and its old leaf came back");
    TEST_EXPECT_EQ(r3, FAULT_HANDLED, "the refunded page re-faults");
    TEST_EXPECT_EQ(dc3, dc2, "as a payer leaf, free");
}

// Two sharers in two address spaces fault one slot, each under its own lock:
// the loser frees its page and refunds its mint, so the record stays the
// resident count and the payer is charged once. The probe runs the rival's
// fault between the loser's charge and its install.
static struct Proc     *g_sa_rival;
static u64              g_sa_rival_va;
static int              g_sa_probe_fired;
static enum fault_result g_sa_rival_r;

static void sa_rival_probe(struct Proc *p, struct Burrow *v, size_t slot) {
    (void)p; (void)v; (void)slot;
    g_shared_mint_probe_for_test = NULL;            // the rival's own mint must not recurse
    g_sa_probe_fired++;
    g_sa_rival_r = sa_fault(g_sa_rival, g_sa_rival_va, true);
}

void test_shared_anon_install_loser_refunds(void) {
    struct Proc *p = proc_alloc(), *q = proc_alloc();
    if (!p || !q) { sa_drop(q); sa_drop(p); TEST_ASSERT(false, "proc_alloc"); }
    u64 sva = sa_map(p, true);
    struct Burrow *sb = sva ? sa_burrow(p, sva) : NULL;
    const bool pfl = sa_floor(p->as);
    struct AddrSpace *qown = NULL;
    struct AddrSpace *c = sva ? sa_fork(p, q, &qown) : NULL;
    bool cfl = false, same_pa = false;
    u32 d0 = 0, c0 = 0, rec = 0, res = 0, dp = 0, dc = 0;
    enum fault_result r = FAULT_UNHANDLED_USER;
    g_sa_rival_r = FAULT_UNHANDLED_USER;
    g_sa_probe_fired = 0;
    if (c) {
        cfl = sa_floor(c);
        d0 = sa_data(p->as); c0 = sa_data(c);
        g_sa_rival = q;
        g_sa_rival_va = sva;
        g_shared_mint_probe_for_test = sa_rival_probe;
        r = sa_fault(p, sva, true);                     // loses to the rival it runs
        g_shared_mint_probe_for_test = NULL;
        g_sa_rival = NULL;
        rec = burrow_payer_charge_pages_for_test(sb);
        res = burrow_lazy_resident_count(sb);
        dp = sa_data(p->as); dc = sa_data(c);
        paddr_t pa = sa_leaf_pa(sa_leaf(p->as->pgtable_root, sva));
        same_pa = pa != 0 && pa == sa_leaf_pa(sa_leaf(c->pgtable_root, sva));
        sa_unfloor(c, cfl);
    }
    sa_unfork(q, qown, c);
    sa_unfloor(p->as, pfl);
    sa_drop(q);
    sa_drop(p);

    TEST_ASSERT(sva != 0 && c != NULL, "the shared mint and the fork");
    TEST_ASSERT(pfl && cfl, "the floors under both counts");
    TEST_EXPECT_EQ(g_sa_probe_fired, 1, "the rival's fault ran inside the loser's mint");
    TEST_EXPECT_EQ(g_sa_rival_r, FAULT_HANDLED, "the rival won the slot");
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "the loser's fault is served too");
    TEST_EXPECT_EQ(res, 1u, "one page resident");
    TEST_EXPECT_EQ(rec, 1u, "the record is the resident count: the loser refunded its mint");
    TEST_EXPECT_EQ(dp, d0 + 1u, "the payer is charged once for the one page");
    TEST_EXPECT_EQ(dc, c0 + 1u, "the rival paid its leaf");
    TEST_ASSERT(same_pa, "both leaves name the winner's page");
}

// A protect splits a shared mapping like any other, each piece keeping the
// payer flag; a fork then maps every piece of the one Burrow, none of them the
// payer's.
void test_shared_anon_protect_split_keeps_payer(void) {
    struct Proc *p = proc_alloc(), *q = proc_alloc();
    if (!p || !q) { sa_drop(q); sa_drop(p); TEST_ASSERT(false, "proc_alloc"); }
    u64 sva = sa_map(p, true);
    struct Burrow *sb = sva ? sa_burrow(p, sva) : NULL;
    s64 prc = sva ? sys_burrow_protect_for_proc(p, sva + PAGE_SIZE, PAGE_SIZE,
                                                BURROW_PROT_READ, 0) : -1;
    struct Vma *a = sva ? vma_lookup(p, sva) : NULL;
    struct Vma *b = sva ? vma_lookup(p, sva + PAGE_SIZE) : NULL;
    struct Vma *e = sva ? vma_lookup(p, sva + 2 * PAGE_SIZE) : NULL;
    bool split = a && b && e && a != b && b != e &&
                 b->prot == VMA_PROT_READ && a->prot == VMA_PROT_RW;
    bool all_payer = split && (a->flags & VMA_FLAG_PAYER) && (b->flags & VMA_FLAG_PAYER) &&
                     (e->flags & VMA_FLAG_PAYER);
    const bool pfl = sa_floor(p->as);
    const u32 d0 = sa_data(p->as);
    enum fault_result r = sva ? sa_fault(p, sva + PAGE_SIZE, false) : FAULT_UNHANDLED_USER;
    const u32 d1 = sa_data(p->as);
    struct AddrSpace *qown = NULL;
    struct AddrSpace *c = split ? sa_fork(p, q, &qown) : NULL;
    bool child_pieces = false;
    if (c) {
        struct Vma *ca = vma_lookup(q, sva), *cb = vma_lookup(q, sva + PAGE_SIZE),
                   *ce = vma_lookup(q, sva + 2 * PAGE_SIZE);
        child_pieces = ca && cb && ce && ca != cb && cb != ce &&
                       ca->burrow == sb && cb->burrow == sb && ce->burrow == sb &&
                       !((ca->flags | cb->flags | ce->flags) & (VMA_FLAG_PAYER | VMA_FLAG_COW));
    }
    sa_unfork(q, qown, c);
    sa_unfloor(p->as, pfl);
    sa_drop(q);
    sa_drop(p);

    TEST_ASSERT(sva != 0, "the shared mint");
    TEST_EXPECT_EQ(prc, (s64)0, "a protect admits a shared mapping");
    TEST_ASSERT(split, "the protect cut it into three pieces, the middle one R");
    TEST_ASSERT(all_payer, "every piece keeps the payer flag");
    TEST_ASSERT(pfl, "the floor under the payer's count");
    TEST_EXPECT_EQ(r, FAULT_HANDLED, "the R piece faults in");
    TEST_EXPECT_EQ(d1, d0 + 1u, "the mint is paid, the payer leaf is not");
    TEST_ASSERT(c != NULL, "the fork");
    TEST_ASSERT(child_pieces, "the child maps every piece of the one Burrow, none the payer's or COW");
}

// MADV_DONTNEED over a shared range drops this space's view and keeps the
// contents: the leaf goes (refunded, for a sharer that is not the payer), the
// page stays, and the next touch maps it again. MADV_FREE and the native
// decommit refuse a shared range (EINVAL), changing nothing. The private map
// is the control: DONTNEED still releases its page.
void test_shared_anon_dontneed_keeps_contents(void) {
    struct Proc *p = proc_alloc(), *q = proc_alloc();
    if (!p || !q) { sa_drop(q); sa_drop(p); TEST_ASSERT(false, "proc_alloc"); }
    u64 sva = sa_map(p, true), pva = sa_map(p, false);
    struct Burrow *sb = sva ? sa_burrow(p, sva) : NULL;
    struct Burrow *pb = pva ? sa_burrow(p, pva) : NULL;
    enum fault_result r0 = sva ? sa_fault(p, sva, true) : FAULT_UNHANDLED_USER;
    struct page *pg = sb ? burrow_lazy_slot_for_test(sb, 0) : NULL;
    if (pg) *(volatile u64 *)pa_to_kva(page_to_pa(pg)) = SA_PATTERN;
    struct AddrSpace *qown = NULL;
    struct AddrSpace *c = (pg && pva) ? sa_fork(p, q, &qown) : NULL;
    bool cfl = false, child_leaf_gone = false, parent_leaf = false, kept = false, back = false;
    u32 c0 = 0, dc1 = 0, dc2 = 0, res = 0;
    s64 dn = -1, fr = 0, nat = 0;
    enum fault_result r1 = FAULT_UNHANDLED_USER, r2 = FAULT_UNHANDLED_USER;
    if (c) {
        cfl = sa_floor(c);
        c0 = sa_data(c);
        r1 = sa_fault(q, sva, false);
        dc1 = sa_data(c);
        dn = sys_burrow_dontneed_core(q, sva, PAGE_SIZE);
        child_leaf_gone = sa_leaf(c->pgtable_root, sva) == 0;
        dc2 = sa_data(c);
        res = burrow_lazy_resident_count(sb);
        parent_leaf = sa_leaf(p->as->pgtable_root, sva) != 0;
        kept = *(volatile u64 *)pa_to_kva(page_to_pa(pg)) == SA_PATTERN;
        fr  = sys_burrow_decommit_core(q, sva, PAGE_SIZE);
        nat = sys_burrow_decommit_for_proc(q, sva, PAGE_SIZE);
        r2 = sa_fault(q, sva, false);
        back = sa_leaf_pa(sa_leaf(c->pgtable_root, sva)) == page_to_pa(pg);
        sa_unfloor(c, cfl);
    }
    sa_unfork(q, qown, c);
    // The control, in the parent: a private page's DONTNEED releases it.
    enum fault_result rp = pva ? sa_fault(p, pva, true) : FAULT_UNHANDLED_USER;
    u32 pres0 = pb ? burrow_lazy_resident_count(pb) : 0;
    s64 pdn = pva ? sys_burrow_dontneed_core(p, pva, PAGE_SIZE) : -1;
    u32 pres1 = pb ? burrow_lazy_resident_count(pb) : 99;
    sa_drop(q);
    sa_drop(p);

    TEST_ASSERT(sva != 0 && pva != 0 && pg != NULL, "the mints and the shared page");
    TEST_EXPECT_EQ(r0, FAULT_HANDLED, "the payer mints");
    TEST_ASSERT(c != NULL && cfl, "the fork, and the floor under the child");
    TEST_EXPECT_EQ(r1, FAULT_HANDLED, "the child's hit");
    TEST_EXPECT_EQ(dc1, c0 + 1u, "the child paid its leaf");
    TEST_EXPECT_EQ(dn, (s64)0, "MADV_DONTNEED over a shared range: 0");
    TEST_ASSERT(child_leaf_gone, "the child's view of the page is gone");
    TEST_EXPECT_EQ(dc2, c0, "and its leaf refunded");
    TEST_EXPECT_EQ(res, 1u, "the page stays in the Burrow");
    TEST_ASSERT(parent_leaf, "the parent's leaf is untouched");
    TEST_ASSERT(kept, "the contents are kept");
    TEST_EXPECT_EQ(fr, -(s64)T_E_INVAL, "MADV_FREE's core refuses a shared range: EINVAL");
    TEST_EXPECT_EQ(nat, (s64)-1, "and so does the native decommit");
    TEST_EXPECT_EQ(r2, FAULT_HANDLED, "the next touch re-faults");
    TEST_ASSERT(back, "and reads the page that is there");
    TEST_EXPECT_EQ(rp, FAULT_HANDLED, "control: the private page faults in");
    TEST_EXPECT_EQ(pres0, 1u, "control: one private page resident");
    TEST_EXPECT_EQ(pdn, (s64)0, "control: DONTNEED over the private range");
    TEST_EXPECT_EQ(pres1, 0u, "control: DONTNEED still releases a private page");
}
