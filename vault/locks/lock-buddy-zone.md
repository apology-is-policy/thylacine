---
id: lock-buddy-zone
type: lock
title: "buddy_zone.lock — the free-list lock, a deep leaf"
kind: spin-irqsave
guards: "the per-order free lists, free_pages_per_order[], total_free_pages, and every struct-page flags/order transition through split/merge"
orders-before: []
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

One per zone (one zone at v1.0: `g_zone0`). Taken irqsave around
`alloc_locked` / `free_locked` / `buddy_free_region` — split, merge,
and list surgery only. Nothing under it allocates, sleeps, or takes
any other lock: it is a **deep leaf**, which is exactly why so many
chains may end on it:

- bare IRQ mask → zone (magazine refill/drain — 8 separate
  acquisitions per crossing, [[seam-buddy-bulk-op]])
- [[lock-kmem-cache]] → zone (slab_new / slab_drain)
- `vma_lock` → zone (demand paging; the documented tree-wide edge)
- the Larder leaf → zone (page-buffer frees)
- `torpor_lock` → `vma_lock` → zone (the rare decommit-window
  re-fault only — [[sub-kernel-torpor]])

## Held across

Split and merge loops — bounded by MAX_ORDER (18) iterations. Never
across `KP_ZERO` zeroing (that runs after release, on pages private
to the caller) and never across the magazine layer (which masks IRQs
but takes no lock of its own).

## Prosecution

- Any new acquisition under this lock is a lock-order event —
  the leaf property is load-bearing for every chain above.
- `buddy_free`'s order > MAX_ORDER extinction (F37) must stay ahead
  of the merge loop: the loop indexes `free_lists[order]` and a
  corrupted order walks off the array.
- The #808 boot page-map allocates page-table pages FROM this zone
  while mapping it — sound only in the single-CPU IRQ-masked boot
  window it runs in.
