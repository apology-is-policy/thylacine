---
id: fnd-rw1-fs1
type: fnd
round: adt-rw1-mm-r1
severity: P2
status: fixed
title: "kmem_cache_destroy's guard tested nr_full — a live object on a PARTIAL slab was freed silently"
surface: [sub-kernel-mm-slub]
threatens: []
fixed-by: chg-2026-06-10-rw1-allocator
regression: "kmem_cache_live_count + the destroy extinction on any live object"
created: 2026-08-01
---
## Prosecution

Destroy drains every partial slab's page back to the buddy
unconditionally. The pre-fix guard extincted only on `nr_full != 0`
— so a cache destroyed with a live object on a PARTIAL slab passed
the guard, and that object's page went back to the buddy and got
recycled under it: a silent use-after-free with an arbitrary fuse.
The dangerous case was quiet while the LESS dangerous one (full
slabs, which the drain doesn't even touch) was loud.

## Fix

The guard is `alloc_count - free_count != 0` — the exact live count
across full AND partial slabs, subsuming the old check. Exposed as
`kmem_cache_live_count` for diagnostics.

The batch-8-pinned lesson two months early: the guard that exists is
what stops you asking whether it is the right guard.
