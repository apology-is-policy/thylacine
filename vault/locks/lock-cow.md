---
id: lock-cow
type: lock
title: "g_cow_lock — the global copy-on-write share count leaf"
kind: spin
guards: "every page's cow_share count, and the copy-on-write break's drop-decide-act as one step"
orders-before: []
created: 2026-08-06
updated: 2026-08-06
---
## Discipline

**A single global spinlock, and being global is the point rather than a
concession.** Two sharers of one copy-on-write page hold *different*
Burrow locks — a fork clones the Burrow per address space — so no
per-Burrow lock can serialise the break's decide. What the protocol
requires is only that drop-decide-act be **one step**, and a global leaf
provides it. Plan 9 serialises `Page.ref` under the global `palloc.lock`
for the same reason.

**It is a leaf: nothing is taken under it.** The established order above it
is `as->lock -> v->lock -> g_cow_lock` ([[lock-vma]] then
[[lock-burrow]]). No path allocates, frees, sleeps, or touches a page
table while holding it.

**Held across the decide only** — never across the copy or the allocation.
`cow_page_break_is_sole` takes it, compares, and releases; the caller then
allocates and copies outside. `cow_page_put` likewise returns the free
*decision* and the caller frees outside the lock. Returning the decision
rather than a queryable count is deliberate: a caller that could read the
count, release, and then act would race a peer in between, which is the
whole failure [[spec-cow]]'s `BUGGY_BREAK_UNLOCKED` counterexample
demonstrates.

Every accessor **extincts on a zero count** rather than proceeding. Zero
means some site put a page into an anon Burrow slot without calling
`cow_page_set_sole`, so the count belongs to a previous owner and any free
decision computed from it would free a page another address space still
maps. Failing loud is the intended behaviour, not a guard against a live
path.

Contention is only between concurrent breaks, each holding the lock for a
single compare. **A per-page hashed lock array is the recorded seam** if
this ever measures, and is deliberately not taken now.

`cow_page_share_for_test` takes the lock to read the count and is for
tests and diagnostics only — a read that is *acted upon* is exactly the
race the other three entry points exist to prevent.
