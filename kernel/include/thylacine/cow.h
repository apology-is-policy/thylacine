// kernel/include/thylacine/cow.h -- the copy-on-write page share count
// (LINEAGE L-4b; docs/LINEAGE.md section 5.4's L-4b correction; ARCH section 28
// I-44). Scripture: 8008a65c.
//
// WHY THE COUNT IS ON THE PAGE
//
// A COW break has to put the private page somewhere. It cannot go in a shared
// Burrow's slot -- another sharer still needs the pristine page there -- and it
// cannot be owned by the PTE alone, because nothing in this tree frees user
// pages from a page table (vma_drain frees Vma structs and drops mapping refs;
// proc_pgtable_destroy frees TABLE pages), so a PTE-owned page would leak at
// teardown. So a fork CLONES the Burrow per address space -- same size, its own
// filepages[], the same page pointers -- which is Plan 9's dupseg.
//
// After which a count indexed by SLOT cannot work: a break makes my slot and
// the page the count describes diverge, so a later fork bumps an entry covering
// two different pages. Take-in-place survives that (the entry is the SUM over
// the groups sharing it, so == 1 still implies a sole holder), but the FREE
// decision corrupts -- one group drives the entry to zero and frees its page,
// leaving another group's page at zero to leak or underflow. Freeing a shared
// page requires knowing how many holders remain, and that is a fact about the
// PAGE.
//
// THE LOCK IS GLOBAL, AND THAT IS THE POINT
//
// Two sharers of one page hold DIFFERENT Burrow locks, so no per-Burrow lock
// could serialise the break's decide. specs/cow.tla says "under the Burrow
// lock", but its actual requirement is that drop-decide-act be ONE step -- that
// is exactly what its BUGGY_BREAK_UNLOCKED variant splits, letting two sharers
// each drop and then both read zero -- and a global leaf lock provides it. Plan
// 9 serialises Page.ref under the global palloc.lock for the same reason.
//
// g_cow_lock is a LEAF: nothing is taken under it. The established order above
// it is as->lock -> v->lock -> g_cow_lock, and no path allocates, frees, sleeps
// or touches a page table while holding it. It is held across the decide only,
// never across the copy or the allocation.
//
// A per-page hashed lock array is the recorded seam if this ever measures. It
// is deliberately NOT taken now: the contention is only between concurrent COW
// breaks, each of which holds the lock for a single compare.
//
// THE CONTRACT (page.h states it too, next to the field)
//
//   cow_share is meaningful ONLY while the page sits in an anon Burrow's
//   filepages[] slot, and it is ESTABLISHED, NEVER INHERITED.
//
// A page recycled through the buddy carries whatever its last owner left, so
// every site that puts a page into such a slot calls cow_page_set_sole. That is
// what keeps this field from becoming the hazard LINEAGE section 2.8 names --
// "a field whose name states a contract nothing keeps" -- which is why
// cow_page_get / put / break_is_sole all EXTINCT on a zero count rather than
// guessing: zero means some site put a page into a slot without establishing
// it, and continuing would free a page another address space still maps.

#ifndef THYLACINE_COW_H
#define THYLACINE_COW_H

#include <thylacine/types.h>

struct page;

// Establish this page as solely held: cow_share = 1. Called by EVERY site that
// puts a page into an anon Burrow's filepages[] slot -- the closed set is
// burrow_lazy_populate, the demand-zero fault install, and the COW break's
// private page. Idempotent in effect but not in meaning: it OVERWRITES, which
// is the point, because the previous value belongs to a previous owner.
void cow_page_set_sole(struct page *pg);

// One more Burrow slot points at this page (the fork clone). Extincts on a
// zero count -- see the contract above.
void cow_page_get(struct page *pg);

// One fewer Burrow slot points at this page. Returns true when that was the
// LAST holder, in which case the caller owns the page and must free it --
// outside this lock, per the leaf discipline.
//
// The return is the free decision itself rather than a query, so a caller
// cannot read the count, act on it, and race a peer in between.
bool cow_page_put(struct page *pg);

// The break's decide, as ONE atomic step (specs/cow.tla::DecideLocked).
//
// true  -- the caller is the SOLE holder: take the page IN PLACE and re-install
//          it writable. No copy, no count change. This is the common case after
//          one side of a fork execs, and it is safe against a concurrent fork
//          bumping the count because only a HOLDER can fork, the caller is the
//          only holder, and the caller is here.
//
// false -- the caller must allocate, copy, and install privately. Its own share
//          stays HELD across the copy, and that retained share IS the model's
//          pin: the page cannot reach zero holders while a breaker is still
//          reading it (cow.tla::BUGGY_TEARDOWN_NO_PIN is the counterexample --
//          drop first and a concurrent exit frees the page mid-copy). The
//          caller calls cow_page_put only once the copy is DONE.
bool cow_page_break_is_sole(struct page *pg);

// Read the count. FOR TESTS AND DIAGNOSTICS ONLY -- a read that is acted upon
// is the race cow_page_break_is_sole and cow_page_put exist to prevent.
u32 cow_page_share_for_test(const struct page *pg);

#endif // THYLACINE_COW_H
