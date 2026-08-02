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
#include <thylacine/types.h>

#include "../../mm/magazines.h"
#include "../../mm/phys.h"
#include "test.h"

void test_cow_set_sole_never_inherits(void);
void test_cow_get_put_reports_last_holder(void);
void test_cow_break_decide_is_sole(void);
void test_burrow_lazy_free_is_conditional_on_share(void);

// One page's worth of Burrow -- enough for every property here, and it keeps
// the filepages[] allocation identical between the two differential runs.
#define COW_ONE_PAGE  4096ull

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
    struct AddrSpace *as = addrspace_alloc();
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
