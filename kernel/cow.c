// kernel/cow.c -- the copy-on-write page share count (LINEAGE L-4b; I-44).
// Scripture: 8008a65c. Model: specs/cow.tla.
//
// The rationale for the count living on struct page, and for this lock being
// global rather than per-Burrow, is in cow.h. This file is deliberately tiny:
// it is a counter and one decide, and every caller's correctness rests on the
// decide being a single step, so there is nothing here worth being clever with.

#include <thylacine/cow.h>
#include <thylacine/extinction.h>
#include <thylacine/page.h>
#include <thylacine/spinlock.h>

// The LEAF lock. Order above it is as->lock -> v->lock -> g_cow_lock; nothing
// is taken, allocated, freed or slept on beneath it.
static spin_lock_t g_cow_lock;

// A page reaching any of these with cow_share == 0 means some site put it into
// an anon Burrow slot without establishing the count -- the contract violation
// cow.h describes. Continuing would hand out a free decision computed from a
// previous owner's value, so this fails LOUD rather than guessing.
static void cow_require_established(const struct page *pg) {
    if (pg->cow_share == 0)
        extinction("cow: page in an anon Burrow slot has cow_share == 0 "
                   "(a site put it there without cow_page_set_sole -- see the "
                   "ESTABLISHED-NEVER-INHERITED contract in cow.h)");
}

void cow_page_set_sole(struct page *pg) {
    if (!pg)
        extinction("cow_page_set_sole(NULL)");
    spin_lock(&g_cow_lock);
    // OVERWRITE, never read-modify-write: whatever is here belongs to whoever
    // owned this page before the buddy handed it back.
    pg->cow_share = 1;
    spin_unlock(&g_cow_lock);
}

void cow_page_get(struct page *pg) {
    if (!pg)
        extinction("cow_page_get(NULL)");
    spin_lock(&g_cow_lock);
    cow_require_established(pg);
    // A u32 overflow would need 2^32 address spaces sharing one page, which
    // PROC_CHILD_MAX and the I-32 axes bound far below; the check is a drift
    // alarm, not a live path.
    if (pg->cow_share == 0xFFFFFFFFu)
        extinction("cow_page_get: share count overflow");
    pg->cow_share++;
    spin_unlock(&g_cow_lock);
}

bool cow_page_put(struct page *pg) {
    if (!pg)
        extinction("cow_page_put(NULL)");
    spin_lock(&g_cow_lock);
    cow_require_established(pg);
    bool last = (--pg->cow_share == 0);
    spin_unlock(&g_cow_lock);
    return last;                    // caller frees OUTSIDE the lock (leaf order)
}

bool cow_page_break_is_sole(struct page *pg) {
    if (!pg)
        extinction("cow_page_break_is_sole(NULL)");
    spin_lock(&g_cow_lock);
    cow_require_established(pg);
    // The whole decide, in one step. On the sole path the count is deliberately
    // NOT touched -- taking the page in place leaves exactly one holder, which
    // is what it already says. On the copy path it is deliberately not dropped
    // either: the caller's retained share is the pin that keeps a concurrent
    // exit from freeing the page mid-copy (cow.tla::BUGGY_TEARDOWN_NO_PIN).
    bool sole = (pg->cow_share == 1);
    spin_unlock(&g_cow_lock);
    return sole;
}

u32 cow_page_share_for_test(const struct page *pg) {
    if (!pg) return 0;
    spin_lock(&g_cow_lock);
    u32 v = pg->cow_share;
    spin_unlock(&g_cow_lock);
    return v;
}
