---
id: lock-kmem-cache
type: lock
title: "kmem_cache.lock — per-cache slab serialization"
kind: spin-irqsave
guards: "the cache's partial/full lists + counts, every slab's freelist head and inuse count, and the cumulative alloc/free counters"
orders-before: [lock-buddy-zone]
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

One per `kmem_cache`. Taken irqsave for the whole of
`kmem_cache_alloc` / `kmem_cache_free` — including the nested
`alloc_pages` (slab_new) and `free_pages` (slab_drain), so
kmem-cache → buddy-zone is a standing order edge. `KP_ZERO`'s
object zeroing runs AFTER release (the object is the caller's).

`kmem_cache_destroy` deliberately runs WITHOUT this lock — the
caller-quiesce contract (RW-1 F-S2): the live-count check and the
partial-drain assume no concurrent alloc/free on any CPU.

## Held across

The buddy zone lock (nested irqsave — fine, the zone is a leaf).
Never across the cache-list lock: [[lock-cache-list]] is taken alone.

## Prosecution

- The destroy path's locklessness is a CONTRACT, not an oversight —
  adding the lock there without removing the quiesce requirement
  would hide, not fix, a concurrent-destroy race.
- Nothing under this lock may reach the cache LIST (the leaf
  discipline runs the other way: list operations never nest cache
  locks).
