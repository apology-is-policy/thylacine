// LINEAGE L-4b: the per-page copy-on-write share count.
//
//   cow.set_sole_never_inherits
//     THE contract test. A page fresh from the buddy carries whatever its last
//     owner left in cow_share, so every site that puts a page into an anon
//     Burrow slot must ESTABLISH the count rather than assume it. This drives a
//     deliberately-poisoned value through cow_page_set_sole and pins that it is
//     overwritten -- the property that keeps the field from becoming the
//     LINEAGE section 2.8 hazard it was named to avoid ("a field whose name
//     states a contract nothing keeps"), where a stale count means a premature
//     free or a leak.
//
//   cow.get_put_reports_last_holder
//     The free decision IS the return value, not a separate query: put reports
//     false while other holders remain and true exactly once, for the last one.
//     A caller that instead read the count and then acted would be racing.
//
//   cow.break_decide_is_sole
//     The break's decide (specs/cow.tla::DecideLocked): sole holder -> take in
//     place; a co-holder -> must copy. Also pins that deciding does NOT mutate
//     the count -- the copy path keeps its share HELD across the copy, and that
//     retained share is the model's pin (cow.tla::BUGGY_TEARDOWN_NO_PIN is what
//     dropping it early looks like).
//
//   burrow.lazy_free_is_conditional_on_share
//     The integration, and the one that would catch a real regression: tearing
//     down a Burrow whose page is co-held must NOT return that page to the
//     buddy. Asserted as a DIFFERENTIAL between two identical teardowns that
//     differ only in whether a co-holder exists, because the absolute free-page
//     delta also includes the filepages[] kfree -- which cancels between the two
//     runs. (An earlier draft asserted on PG_FREE instead. That is a trap: the
//     buddy's coalesce anchors on the LOWER-pfn buddy, so a freed page that
//     merges rightward never gets the flag set on its own struct page, and the
//     assertion would have passed vacuously.)
//
//     AND THE INSTRUMENT NEEDED A DRAIN, which is worth knowing before writing
//     any other page-accounting test: an order-0 free does NOT reach the buddy.
//     mag_free pushes it onto a per-CPU magazine (mm/magazines.c), so
//     phys_free_pages() is blind to it -- measured, this test's first run
//     reported delta 0 for BOTH the shared and the private teardown, which
//     looks exactly like "the conditional free is broken" and was in fact "the
//     instrument cannot see either free". magazines_drain_all() before each
//     sample makes phys_free_pages() a true total again; with it, the private
//     run returns 1 page and the shared run returns 0.

#include <thylacine/addrspace.h>
#include <thylacine/burrow.h>
#include <thylacine/cow.h>
#include <thylacine/page.h>
#include <thylacine/proc.h>
#include <thylacine/types.h>
#include <thylacine/vma.h>

#include "../../arch/arm64/fault.h"
#include "../../mm/magazines.h"
#include "../../mm/phys.h"
#include "test.h"

// L-4b-2: the producer of sharing.
//
//   cow.burrow_clone_shares_resident_pages
//     dupseg: a SEPARATE Burrow whose slots point at the SAME pages, one extra
//     share each. Pins the two properties the break's locking rests on -- the
//     clone is a distinct object (so each address space owns a slot it may
//     overwrite) and a non-resident slot stays NULL rather than being materialized
//     (an untouched page reads as zero, and each side demand-zeroes its own).
//
//   cow.addrspace_clone_shares_and_writeprotects
//     The address-space half: the child gets its own Burrow over the same page,
//     BOTH VMAs are flagged, the child is charged for the page it now maps, and --
//     the load-bearing half -- the PARENT's already-installed writable PTE is
//     GONE. Leaving it is the I-44 violation: the parent would write through a
//     stale writable translation into a page the child can read.
//
//   cow.addrspace_clone_refuses_and_leaves_parent_intact
//     An eager BURROW_TYPE_ANON VMA has no per-page ownership to break, so the
//     fork is refused whole. Also pins that a REFUSED clone leaves the parent
//     completely untouched -- no half-flagged VMAs, no gratuitously dropped PTEs
//     -- which is why the flag/write-protect pass runs only on success.

void test_cow_set_sole_never_inherits(void);
void test_cow_get_put_reports_last_holder(void);
void test_cow_break_decide_is_sole(void);
void test_burrow_lazy_free_is_conditional_on_share(void);
void test_cow_burrow_clone_shares_resident_pages(void);
void test_cow_addrspace_clone_shares_and_writeprotects(void);
void test_cow_addrspace_clone_refuses_and_leaves_parent_intact(void);
void test_cow_clone_shares_readonly_eager_anon(void);
void test_cow_break_read_then_write_copies(void);
void test_cow_break_sole_holder_takes_in_place(void);

// One page's worth of Burrow -- enough for every property here, and it keeps
// the filepages[] allocation identical between the two differential runs.
#define COW_ONE_PAGE  4096ull

// A user VA well inside the user half, away from every other test's range.
#define COW_TEST_VA   0x20000000ull

// Walk the per-address-space tree to the L3 leaf covering `vaddr`. Returns the
// raw descriptor, or 0 when any level is missing / not a table -- so "the PTE is
// gone" and "the PTE was never there" read the same, which is exactly what the
// write-protect assertions want to say.
static u64 cow_test_pte(paddr_t pgtable_root, u64 vaddr) {
    const u64 VALID = 1ull << 0, TABLE = 1ull << 1;
    u64 *t = (u64 *)pa_to_kva(pgtable_root);
    for (int lvl = 0; lvl < 3; lvl++) {
        u64 e = t[(vaddr >> (39 - 9 * lvl)) & 0x1ff];
        if (!(e & VALID) || !(e & TABLE)) return 0;
        t = (u64 *)pa_to_kva(e & 0x0000FFFFFFFFF000ull);
    }
    u64 leaf = t[(vaddr >> 12) & 0x1ff];
    return (leaf & VALID) ? leaf : 0;
}

// AP[2:1] at PTE bits 7:6 -- 0b01 user-RW, 0b11 user-RO.
#define COW_AP_FIELD   (3ull << 6)
#define COW_AP_RW_ANY  (1ull << 6)
#define COW_AP_RO_ANY  (3ull << 6)

static void cow_make_fi(struct fault_info *fi, u64 vaddr, bool is_write) {
    fi->vaddr          = vaddr;
    fi->elr            = 0;
    fi->esr            = 0;
    fi->ec             = 0x24;          // EC_DATA_ABORT_LOWER
    fi->fsc            = 0x07;          // FSC_TRANS_FAULT_L3
    fi->fault_level    = 3;
    fi->from_user      = true;
    fi->is_instruction = false;
    fi->is_write       = is_write;
    fi->is_translation = true;
    fi->is_permission  = false;
    fi->is_access_flag = false;
}

static void drop_proc_for_cow(struct Proc *p) {
    if (!p) return;
    p->state = 2;                        // PROC_STATE_ZOMBIE
    proc_free(p);
}

void test_cow_set_sole_never_inherits(void) {
    struct page *pg = alloc_pages(0, KP_ZERO);
    TEST_ASSERT(pg != NULL, "alloc_pages for the contract test");

    // Poison the field the way a previous owner would have left it. 77 is
    // arbitrary; what matters is that it is neither 0 nor 1, so a set_sole that
    // silently kept or incremented the old value would be visible.
    pg->cow_share = 77;
    cow_page_set_sole(pg);
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 1ull,
                   "set_sole ESTABLISHES 1 over a previous owner's value");

    // And from the value the buddy leaves after a plain free/alloc cycle.
    pg->cow_share = 0;
    cow_page_set_sole(pg);
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 1ull,
                   "set_sole establishes 1 from a zeroed field too");

    TEST_ASSERT(cow_page_put(pg), "the sole holder's put reports last");
    free_pages(pg, 0);
}

void test_cow_get_put_reports_last_holder(void) {
    struct page *pg = alloc_pages(0, KP_ZERO);
    TEST_ASSERT(pg != NULL, "alloc_pages");
    cow_page_set_sole(pg);

    cow_page_get(pg);                       // a fork: a second Burrow slot
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 2ull, "get -> 2 holders");
    cow_page_get(pg);                       // and a third
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 3ull, "get -> 3 holders");

    TEST_ASSERT(!cow_page_put(pg), "put with holders left does NOT report last");
    TEST_ASSERT(!cow_page_put(pg), "put with one holder left does NOT report last");
    TEST_ASSERT(cow_page_put(pg),  "the LAST put reports last -- exactly once");

    free_pages(pg, 0);
}

void test_cow_break_decide_is_sole(void) {
    struct page *pg = alloc_pages(0, KP_ZERO);
    TEST_ASSERT(pg != NULL, "alloc_pages");
    cow_page_set_sole(pg);

    TEST_ASSERT(cow_page_break_is_sole(pg),
                "a sole holder breaks IN PLACE (no copy)");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 1ull,
                   "deciding does not mutate the count on the sole path");

    cow_page_get(pg);                       // now co-held
    TEST_ASSERT(!cow_page_break_is_sole(pg),
                "a co-held page must be COPIED, not taken in place");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 2ull,
                   "deciding does not drop the share -- the retained share IS "
                   "the pin that keeps a concurrent exit from freeing the page "
                   "mid-copy (cow.tla::BUGGY_TEARDOWN_NO_PIN)");

    TEST_ASSERT(!cow_page_put(pg), "co-holder put");
    TEST_ASSERT(cow_page_put(pg),  "last put");
    free_pages(pg, 0);
}

// Populate one slot of a fresh lazy Burrow and return its page, or NULL.
static struct page *cow_test_make_populated(struct AddrSpace *as,
                                            struct Burrow **v_out) {
    struct Burrow *v = burrow_create_anon_lazy(COW_ONE_PAGE);
    if (!v) return NULL;
    if (burrow_lazy_populate(as, /*exempt=*/true, v, 0, 1) != 0) {
        burrow_unref(v);
        return NULL;
    }
    *v_out = v;
    return burrow_lazy_slot_for_test(v, 0);
}

void test_burrow_lazy_free_is_conditional_on_share(void) {
    struct AddrSpace *as = addrspace_alloc(PROC_PAGE_MAX);
    TEST_ASSERT(as != NULL, "addrspace_alloc");

    // Run A -- NOT co-held. Tearing the Burrow down returns its page.
    struct Burrow *va = NULL;
    struct page *pa = cow_test_make_populated(as, &va);
    TEST_ASSERT(pa != NULL, "run A: populate");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pa), 1ull,
                   "populate establishes the page as solely held");
    magazines_drain_all();
    u64 before_a = phys_free_pages();
    burrow_unref(va);
    magazines_drain_all();
    u64 delta_a = phys_free_pages() - before_a;

    // Run B -- co-held, as a fork's second Burrow would hold it. Identical
    // teardown, so the filepages[] kfree contributes identically and cancels.
    struct Burrow *vb = NULL;
    struct page *pb = cow_test_make_populated(as, &vb);
    TEST_ASSERT(pb != NULL, "run B: populate");
    cow_page_get(pb);                       // the simulated co-holder
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pb), 2ull, "run B is co-held");
    magazines_drain_all();
    u64 before_b = phys_free_pages();
    burrow_unref(vb);
    magazines_drain_all();
    u64 delta_b = phys_free_pages() - before_b;

    // THE claim: the co-held teardown returns exactly one page FEWER. Stated as
    // the difference so nothing depends on what else the teardown frees.
    TEST_EXPECT_EQ(delta_a, delta_b + 1,
                   "a co-held page survives its Burrow's teardown: the shared "
                   "run returns exactly one page fewer than the private run");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pb), 1ull,
                   "and the surviving page is left solely held by the co-holder");

    // The co-holder releases: now it frees.
    TEST_ASSERT(cow_page_put(pb), "the co-holder's put reports last");
    free_pages(pb, 0);
    addrspace_unref(as);
}

// =============================================================================
// L-4b-2: the clone.
// =============================================================================

void test_cow_burrow_clone_shares_resident_pages(void) {
    struct AddrSpace *as = addrspace_alloc(PROC_PAGE_MAX);
    TEST_ASSERT(as != NULL, "addrspace_alloc");

    // Four slots; make 0 and 2 resident, leave 1 and 3 untouched.
    struct Burrow *src = burrow_create_anon_lazy(4 * COW_ONE_PAGE);
    TEST_ASSERT(src != NULL, "burrow_create_anon_lazy");
    TEST_EXPECT_EQ(burrow_lazy_populate(as, /*exempt=*/true, src, 0, 1), 0, "populate slot 0");
    TEST_EXPECT_EQ(burrow_lazy_populate(as, /*exempt=*/true, src, 2, 1), 0, "populate slot 2");
    struct page *p0 = burrow_lazy_slot_for_test(src, 0);
    struct page *p2 = burrow_lazy_slot_for_test(src, 2);
    TEST_ASSERT(p0 && p2, "both populated slots resident");

    struct Burrow *clone = burrow_clone_cow(src);
    TEST_ASSERT(clone != NULL, "burrow_clone_cow");
    TEST_ASSERT(clone != src, "the clone is a SEPARATE Burrow -- each address "
                              "space needs a slot it may overwrite at the break");
    TEST_EXPECT_EQ((u64)clone->page_count, (u64)src->page_count, "same page count");
    TEST_EXPECT_EQ((u64)clone->size, (u64)src->size, "same size");

    // Same pages, one more holder each.
    TEST_ASSERT(burrow_lazy_slot_for_test(clone, 0) == p0, "clone slot 0 is the SAME page");
    TEST_ASSERT(burrow_lazy_slot_for_test(clone, 2) == p2, "clone slot 2 is the SAME page");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(p0), 2ull, "slot-0 page now has 2 holders");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(p2), 2ull, "slot-2 page now has 2 holders");

    // Untouched slots stay untouched: a page that was never written reads as
    // zero, so each side demand-zeroes its own after the fork.
    TEST_ASSERT(burrow_lazy_slot_for_test(clone, 1) == NULL, "clone slot 1 stays NULL");
    TEST_ASSERT(burrow_lazy_slot_for_test(clone, 3) == NULL, "clone slot 3 stays NULL");

    // Tearing down the clone leaves the source's pages alive and solely held.
    burrow_unref(clone);
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(p0), 1ull,
                   "the clone's teardown returns slot 0 to a single holder");
    TEST_ASSERT(burrow_lazy_slot_for_test(src, 0) == p0, "and the source still holds it");

    burrow_unref(src);
    addrspace_unref(as);
}

void test_cow_addrspace_clone_shares_and_writeprotects(void) {
    struct Proc *parent = proc_alloc();
    TEST_ASSERT(parent != NULL, "proc_alloc");

    struct Burrow *v = burrow_create_anon_lazy(COW_ONE_PAGE);
    TEST_ASSERT(v != NULL, "burrow_create_anon_lazy");
    TEST_EXPECT_EQ(burrow_map(parent, v, COW_TEST_VA, COW_ONE_PAGE, VMA_PROT_RW), 0,
                   "burrow_map RW");
    burrow_unref(v);                        // the mapping keeps it, as at attach

    // Fault the page in so the parent has a real WRITABLE PTE to lose.
    struct fault_info fi;
    cow_make_fi(&fi, COW_TEST_VA + 0x40, /*is_write=*/true);
    TEST_EXPECT_EQ(userland_demand_page(parent, &fi), FAULT_HANDLED, "parent faults the page in");
    struct page *shared = burrow_lazy_slot_for_test(v, 0);
    TEST_ASSERT(shared != NULL, "parent's slot resident");
    TEST_ASSERT(cow_test_pte(parent->as->pgtable_root, COW_TEST_VA) != 0,
                "parent has a PTE before the fork");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(shared), 1ull, "solely held before the fork");

    struct AddrSpace *child = addrspace_clone(parent->as, /*exempt=*/true);
    TEST_ASSERT(child != NULL, "addrspace_clone");

    // The child maps the same page through its OWN Burrow.
    struct Vma *cv = vma_lookup_in(child, COW_TEST_VA);
    TEST_ASSERT(cv != NULL, "child has the VMA");
    TEST_ASSERT(cv->burrow != v, "child's Burrow is a clone, not the parent's");
    TEST_ASSERT(burrow_lazy_slot_for_test(cv->burrow, 0) == shared,
                "child's slot points at the SAME page");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(shared), 2ull, "two holders after the fork");

    // BOTH sides are flagged -- the parent's write must break too, or it would
    // scribble on a page the child can read.
    struct Vma *pv = vma_lookup_in(parent->as, COW_TEST_VA);
    TEST_ASSERT(pv != NULL, "parent still has its VMA");
    TEST_ASSERT((pv->flags & VMA_FLAG_COW) != 0, "PARENT VMA flagged COW");
    TEST_ASSERT((cv->flags & VMA_FLAG_COW) != 0, "CHILD VMA flagged COW");
    TEST_ASSERT((pv->prot & VMA_PROT_WRITE) != 0,
                "the VMA keeps WRITE -- it is the PTE that goes read-only, or a "
                "COW write would be a segfault instead of a break");

    // THE load-bearing assertion: the parent's stale writable PTE is gone.
    TEST_EXPECT_EQ(cow_test_pte(parent->as->pgtable_root, COW_TEST_VA), 0ull,
                   "the fork DROPPED the parent's writable PTE -- leaving it lets "
                   "the parent write into the child's memory (I-44)");

    // The child is charged for the page it now maps (each address space maps it,
    // so each counts it -- and charging at fork is what makes the break itself
    // unable to fail on the cap).
    TEST_EXPECT_EQ((u64)child->page_count, 1ull, "child charged for the resident page");

    addrspace_unref(child);
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(shared), 1ull,
                   "the child's teardown returns the page to a single holder");
    drop_proc_for_cow(parent);
}

// #136: a READ-ONLY eager-ANON VMA is SHARED, not refused and not cloned.
//
// This is the vDSO clock page, and it is in EVERY EL0 address space -- so before
// this arm existed addrspace_clone refused every real fork, and only the synthetic
// spaces these tests BUILD (which have no vDSO) could be cloned at all. The lesson
// generalises past this bug: a test that constructs its own subject decides what
// the subject contains, and can therefore omit the very thing production always
// has. Hence the pairing below -- read-only shares, writable still refuses -- and
// hence /fork-probe leg K, which forks a REAL address space nobody assembled.
void test_cow_clone_shares_readonly_eager_anon(void) {
    struct Proc *parent = proc_alloc();
    TEST_ASSERT(parent != NULL, "proc_alloc");

    // A lazy VMA so the clone has real COW work to do alongside the eager one.
    struct Burrow *lazy = burrow_create_anon_lazy(COW_ONE_PAGE);
    TEST_ASSERT(lazy != NULL, "burrow_create_anon_lazy");
    TEST_EXPECT_EQ(burrow_map(parent, lazy, COW_TEST_VA, COW_ONE_PAGE, VMA_PROT_RW), 0,
                   "map the lazy VMA");
    burrow_unref(lazy);

    // The vDSO's shape: eager anon, mapped READ-ONLY.
    struct Burrow *ro = burrow_create_anon(COW_ONE_PAGE);
    TEST_ASSERT(ro != NULL, "burrow_create_anon");
    TEST_EXPECT_EQ(burrow_map(parent, ro, COW_TEST_VA + 0x100000ull, COW_ONE_PAGE,
                              VMA_PROT_READ), 0, "map the read-only eager VMA");

    struct AddrSpace *child = addrspace_clone(parent->as, /*exempt=*/true);
    TEST_ASSERT(child != NULL,
                "a read-only eager-ANON VMA must not refuse the fork -- the vDSO "
                "is exactly this, so refusing means no real Proc can fork");

    struct Vma *cv = vma_lookup_in(child, COW_TEST_VA + 0x100000ull);
    TEST_ASSERT(cv != NULL, "child has the read-only VMA");
    TEST_ASSERT(cv->burrow == ro,
                "SHARED, not cloned -- there is nothing to break, so both address "
                "spaces map the one page (the read-only FILE reasoning)");
    TEST_EXPECT_EQ((u64)(cv->flags & VMA_FLAG_COW), 0ull,
                   "and not flagged COW: a shared read-only page has no break");

    struct Vma *pv = vma_lookup_in(parent->as, COW_TEST_VA + 0x100000ull);
    TEST_ASSERT(pv != NULL, "parent still has its read-only VMA");
    TEST_EXPECT_EQ((u64)(pv->flags & VMA_FLAG_COW), 0ull,
                   "the parent's copy is not flagged either");

    // The lazy VMA alongside it still did the COW thing, so this is not passing
    // by the clone having quietly become a no-op.
    struct Vma *pl = vma_lookup_in(parent->as, COW_TEST_VA);
    TEST_ASSERT(pl != NULL && (pl->flags & VMA_FLAG_COW) != 0,
                "the lazy VMA in the same space still went COW");

    addrspace_unref(child);

    // WRITABLE eager anon is still refused -- the pairing that makes the arm a
    // WRITABILITY test rather than a blanket admission of eager anon.
    TEST_EXPECT_EQ(burrow_map(parent, ro, COW_TEST_VA + 0x200000ull, COW_ONE_PAGE,
                              VMA_PROT_RW), 0, "map the SAME Burrow writable");
    burrow_unref(ro);
    TEST_ASSERT(addrspace_clone(parent->as, /*exempt=*/true) == NULL,
                "a WRITABLE eager-ANON VMA must still refuse the fork -- one "
                "indivisible buddy block has no per-page ownership to break");

    drop_proc_for_cow(parent);
}

void test_cow_addrspace_clone_refuses_and_leaves_parent_intact(void) {
    struct Proc *parent = proc_alloc();
    TEST_ASSERT(parent != NULL, "proc_alloc");

    // TWO mappings, and the mix is load-bearing. An earlier draft mapped only the
    // eager one -- and a probe that ungated the flag pass FAILED TO FIRE, because
    // the sole VMA was not a COW candidate, so pass 2 skipped it whether gated or
    // not. The assertion named a property it could not observe. A COW-eligible
    // VMA has to be present for "a failed fork flags nothing" to mean anything.
    //
    // Low VA: a lazy mapping -- cloneable, and therefore flaggable.
    struct Burrow *lazy = burrow_create_anon_lazy(COW_ONE_PAGE);
    TEST_ASSERT(lazy != NULL, "burrow_create_anon_lazy");
    TEST_EXPECT_EQ(burrow_map(parent, lazy, COW_TEST_VA, COW_ONE_PAGE, VMA_PROT_RW), 0,
                   "map the lazy VMA");
    burrow_unref(lazy);

    // High VA: an EAGER anon Burrow -- one indivisible buddy block, so there is no
    // per-page ownership for a break to take, and the fork must be refused whole.
    struct Burrow *eager = burrow_create_anon(COW_ONE_PAGE);
    TEST_ASSERT(eager != NULL, "burrow_create_anon");
    TEST_EXPECT_EQ(burrow_map(parent, eager, COW_TEST_VA + 0x100000ull,
                              COW_ONE_PAGE, VMA_PROT_RW), 0, "map the eager VMA");
    burrow_unref(eager);

    struct fault_info fi;
    cow_make_fi(&fi, COW_TEST_VA, /*is_write=*/true);
    TEST_EXPECT_EQ(userland_demand_page(parent, &fi), FAULT_HANDLED, "lazy page faults in");
    struct page *pg = burrow_lazy_slot_for_test(lazy, 0);
    TEST_ASSERT(pg != NULL, "lazy resident");
    u64 pte_before = cow_test_pte(parent->as->pgtable_root, COW_TEST_VA);
    TEST_ASSERT(pte_before != 0, "parent has a PTE for the lazy page");

    TEST_ASSERT(addrspace_clone(parent->as, /*exempt=*/true) == NULL,
                "an eager-ANON mapping cannot be COW-forked -- refuse whole rather "
                "than hand the child the wrong sharing semantics");

    // The failure path is where the three phases are DISTINGUISHABLE, so this is
    // where they get pinned -- one assertion each, all about the LAZY VMA (the
    // only one any phase touches).
    struct Vma *pv = vma_lookup_in(parent->as, COW_TEST_VA);
    TEST_ASSERT(pv != NULL, "parent still has its lazy VMA");

    // Phase 1 ran, and ran FIRST (#134). It uninstalls ahead of the allocating
    // phase precisely so no page is ever shared with the child while the parent
    // still holds a writable PTE for it -- src->lock cannot establish that on its
    // own, because a peer with an installed PTE stores in hardware and never
    // takes the lock at all. With the uninstall left where it was (after the
    // snapshot) this PTE would still be here.
    TEST_EXPECT_EQ(cow_test_pte(parent->as->pgtable_root, COW_TEST_VA), 0ull,
                   "the uninstall runs BEFORE the clone, so it runs even when "
                   "the clone then fails");

    // Phase 3 did NOT run, which is what "a failed fork leaves the parent intact"
    // means once phase 1 has moved: unflagged, so the faults phase 1 just cost it
    // re-install WRITABLE and it never learns a fork was attempted. The flag could
    // not join phase 1 for exactly this reason -- an uninstall is recoverable by
    // re-faulting, VMA_FLAG_COW is never cleared.
    TEST_EXPECT_EQ((u64)(pv->flags & VMA_FLAG_COW), 0ull,
                   "a FAILED fork flags nothing -- the flag pass runs on success only");

    // ...and the recovery is real, not merely implied: one write fault and the
    // parent is back where it started, writable PTE and all.
    struct fault_info refi;
    cow_make_fi(&refi, COW_TEST_VA, /*is_write=*/true);
    TEST_EXPECT_EQ(userland_demand_page(parent, &refi), FAULT_HANDLED,
                   "the parent re-faults cleanly after a failed fork");
    TEST_EXPECT_EQ(cow_test_pte(parent->as->pgtable_root, COW_TEST_VA), pte_before,
                   "and gets back the SAME writable PTE -- the uninstall cost it "
                   "a fault, not its mapping");

    // And the Burrow the failed attempt DID clone before hitting the eager VMA was
    // released with the discarded child, so the page is solely held again.
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 1ull,
                   "the discarded child returned the share it took");

    drop_proc_for_cow(parent);
}

// =============================================================================
// L-4b-2: the break, driven through the REAL fault arm.
// =============================================================================
//
// Adopting a cloned address space into a second Proc is what fork will do at L-5;
// proc_alloc_in takes its own reference, so the caller drops the one it holds and
// the child owns the space outright.
static struct Proc *cow_adopt(struct AddrSpace *as) {
    struct Proc *p = proc_alloc_in(as, PROC_PAGE_MAX);
    if (p) addrspace_unref(as);          // ref 2 -> 1, held by the child alone
    return p;
}

void test_cow_break_read_then_write_copies(void) {
    struct Proc *parent = proc_alloc();
    TEST_ASSERT(parent != NULL, "proc_alloc parent");

    struct Burrow *pv = burrow_create_anon_lazy(COW_ONE_PAGE);
    TEST_ASSERT(pv != NULL, "burrow_create_anon_lazy");
    TEST_EXPECT_EQ(burrow_map(parent, pv, COW_TEST_VA, COW_ONE_PAGE, VMA_PROT_RW), 0,
                   "burrow_map RW");
    burrow_unref(pv);

    // Fault the page in and stamp it, so "did the copy carry the contents" and
    // "did the parent's page change" are both answerable.
    struct fault_info fi;
    cow_make_fi(&fi, COW_TEST_VA, /*is_write=*/true);
    TEST_EXPECT_EQ(userland_demand_page(parent, &fi), FAULT_HANDLED, "parent faults in");
    struct page *shared = burrow_lazy_slot_for_test(pv, 0);
    TEST_ASSERT(shared != NULL, "resident");
    u64 *pbytes = (u64 *)pa_to_kva(page_to_pa(shared));
    pbytes[0] = 0xC0FFEEull;

    struct AddrSpace *cas = addrspace_clone(parent->as, /*exempt=*/true);
    TEST_ASSERT(cas != NULL, "addrspace_clone");
    struct Proc *child = cow_adopt(cas);
    TEST_ASSERT(child != NULL, "adopt the cloned address space");

    // (a) The child READS first. This must install READ-ONLY -- mapping it
    // writable "because the VMA says writable" is the I-44 violation, and it is
    // also what makes step (b) a regression test rather than a formality.
    cow_make_fi(&fi, COW_TEST_VA + 0x10, /*is_write=*/false);
    TEST_EXPECT_EQ(userland_demand_page(child, &fi), FAULT_HANDLED, "child read fault");
    u64 pte = cow_test_pte(child->as->pgtable_root, COW_TEST_VA);
    TEST_ASSERT(pte != 0, "child has a PTE after the read");
    TEST_EXPECT_EQ(pte & COW_AP_FIELD, COW_AP_RO_ANY,
                   "a READ of a COW page installs READ-ONLY, so the write that "
                   "follows comes back to the break");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(shared),
                   "and it maps the SHARED page -- no copy on a read");

    // (b) Now the child WRITES. The write arrives at a VA that already has a
    // valid read-only PTE, and mmu_install_user_pte REFUSES a mismatching install
    // over one -- so this leg fails outright unless the break clears it first.
    cow_make_fi(&fi, COW_TEST_VA + 0x10, /*is_write=*/true);
    TEST_EXPECT_EQ(userland_demand_page(child, &fi), FAULT_HANDLED,
                   "child write fault BREAKS (a stale read-only PTE must be "
                   "cleared first, or the install is refused and the Proc dies)");

    struct Vma *cvma = vma_lookup_in(child->as, COW_TEST_VA);
    TEST_ASSERT(cvma != NULL, "child VMA");
    struct page *priv = burrow_lazy_slot_for_test(cvma->burrow, 0);
    TEST_ASSERT(priv != NULL, "child slot still resident");
    TEST_ASSERT(priv != shared, "the break gave the child a PRIVATE page");

    pte = cow_test_pte(child->as->pgtable_root, COW_TEST_VA);
    TEST_EXPECT_EQ(pte & COW_AP_FIELD, COW_AP_RW_ANY, "and installed it WRITABLE");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(priv), "PTE maps the private page");

    // The copy carried the contents...
    u64 *cbytes = (u64 *)pa_to_kva(page_to_pa(priv));
    TEST_EXPECT_EQ(cbytes[0], 0xC0FFEEull, "the private page is a COPY, not a fresh zero page");

    // ...and the parent's page is untouched and back to a single holder.
    TEST_ASSERT(burrow_lazy_slot_for_test(pv, 0) == shared, "parent's slot unchanged");
    TEST_EXPECT_EQ(pbytes[0], 0xC0FFEEull, "parent's bytes unchanged");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(shared), 1ull,
                   "the breaker released its share AFTER the copy -- holding it "
                   "across is the pin (cow.tla::BUGGY_TEARDOWN_NO_PIN)");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(priv), 1ull, "the private page is solely held");

    // Divergence: a write to one is invisible to the other.
    cbytes[0] = 0xBEEFull;
    TEST_EXPECT_EQ(pbytes[0], 0xC0FFEEull, "the two address spaces have DIVERGED");

    drop_proc_for_cow(child);
    drop_proc_for_cow(parent);
}

void test_cow_break_sole_holder_takes_in_place(void) {
    u64 free_before;
    struct Proc *parent = proc_alloc();
    TEST_ASSERT(parent != NULL, "proc_alloc");

    struct Burrow *pv = burrow_create_anon_lazy(COW_ONE_PAGE);
    TEST_ASSERT(pv != NULL, "burrow_create_anon_lazy");
    TEST_EXPECT_EQ(burrow_map(parent, pv, COW_TEST_VA, COW_ONE_PAGE, VMA_PROT_RW), 0,
                   "burrow_map RW");
    burrow_unref(pv);

    struct fault_info fi;
    cow_make_fi(&fi, COW_TEST_VA, /*is_write=*/true);
    TEST_EXPECT_EQ(userland_demand_page(parent, &fi), FAULT_HANDLED, "fault in");
    struct page *pg = burrow_lazy_slot_for_test(pv, 0);
    TEST_ASSERT(pg != NULL, "resident");

    // Fork, then let the child go. The VMA stays flagged (the flag is never
    // cleared -- see vma.h), but the page is back to one holder.
    struct AddrSpace *cas = addrspace_clone(parent->as, /*exempt=*/true);
    TEST_ASSERT(cas != NULL, "addrspace_clone");
    addrspace_unref(cas);
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 1ull, "sole holder again");
    struct Vma *pvma = vma_lookup_in(parent->as, COW_TEST_VA);
    TEST_ASSERT(pvma && (pvma->flags & VMA_FLAG_COW) != 0,
                "the VMA is still flagged -- the PAGE count is the truth, the flag "
                "is only the routing");

    // The parent's next write re-faults (the fork dropped its PTE) and must take
    // the page IN PLACE: no copy, no new page, no count change.
    magazines_drain_all();
    free_before = phys_free_pages();
    cow_make_fi(&fi, COW_TEST_VA + 0x20, /*is_write=*/true);
    TEST_EXPECT_EQ(userland_demand_page(parent, &fi), FAULT_HANDLED, "parent re-faults");
    magazines_drain_all();

    TEST_ASSERT(burrow_lazy_slot_for_test(pv, 0) == pg,
                "a SOLE holder takes the page IN PLACE -- copying would be a page "
                "and a page-copy spent to reach the state it is already in");
    TEST_EXPECT_EQ(phys_free_pages(), free_before, "and allocated nothing");
    TEST_EXPECT_EQ((u64)cow_page_share_for_test(pg), 1ull, "count unchanged by the decide");

    u64 pte = cow_test_pte(parent->as->pgtable_root, COW_TEST_VA);
    TEST_EXPECT_EQ(pte & COW_AP_FIELD, COW_AP_RW_ANY, "installed WRITABLE");
    TEST_EXPECT_EQ(pte & 0x0000FFFFFFFFF000ull, page_to_pa(pg), "same page");

    drop_proc_for_cow(parent);
}
