---
id: chg-2026-06-10-rw1-allocator
type: chg
title: "RW-1 allocator fixes: the wrap guard, the list lock, the destroy guard"
date: 2026-06-10
arc: arc-holotype-rw
commits: ["baea64ea", "5b682109"]
touched: [sub-kernel-mm-slub]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

The HOLOTYPE RW-1 review's allocator slice, two commits:

- **A-F1** [P2] ([[fnd-rw1-af1]]) — `kmalloc`'s page-rounding wraps
  for n within `PAGE_SIZE-1` of SIZE_MAX: a near-SIZE_MAX request
  became a 1-page SUCCESS, which also defeated `kcalloc`'s n*size
  overflow guard for size == 1. Now rejected before rounding.
- **A-F2** [P2] ([[fnd-rw1-af2]]) — the global cache list had no
  lock; runtime create/destroy and diagnostic walkers raced the
  splice. `g_cache_list_lock` added as a strict leaf
  ([[lock-cache-list]]).
- **F-S1** [P2] ([[fnd-rw1-fs1]]) — `kmem_cache_destroy`'s guard
  tested only `nr_full`, so a PARTIAL slab holding a live object was
  drained to the buddy silently — page recycled under a live object,
  a slow-fuse UAF — while the less-dangerous full-slab case was
  already loud. The complete check is `alloc_count - free_count`.
- F-S2/F-S3 [P3] — the destroy quiesce contract made explicit; the
  impossible-geometry create pre-guard (align so large that
  objects_per_slab == 0 → NULL-deref at first alloc).

## The shape worth remembering

F-S1 is the audit-table thesis in miniature: the guard that EXISTED
(nr_full) is what stopped anyone asking whether it was the RIGHT
guard — the pinned A-PIN lesson, here two months before V-5d
re-taught it.
