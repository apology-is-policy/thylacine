---
id: chg-2026-05-05-p1id-closing-audit
type: chg
title: "P1-I-D: the Phase-1 closing audit's allocator slice"
date: 2026-05-05
arc: arc-phase1-foundation
commits: ["67b67091"]
touched: [sub-kernel-mm-phys, sub-kernel-mm-slub]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

The Phase-1 exit audit ([[adt-p1id-r1]]) hardened both allocator
layers in one close. The mm-relevant fixes, by finding:

- **F29** — reservation-overlap detection at boot. The motivating
  geometry is real hardware: a Pi 5 with 8 GiB has ~96 MiB of
  struct-pages, easy for firmware to drop the DTB inside — the init
  clear pass would have silently overwritten it.
- **F34** — the low-firmware reservation made an explicit array
  entry instead of cursor arithmetic (composes with F29's check).
- **F32** — `kfree` validates pointer-at-slot-boundary (slab) and
  pointer-at-head-page (large). Interior pointers previously
  corrupted the freelist or fed garbage tail-page metadata to the
  buddy.
- **F33** — explicit full-slab list. Untracked full slabs were
  unreachable at destroy: the slab page AND its objects leaked.
- **F35** — `struct page` size static_assert (the array is a
  per-RAM BSS tax; growth must be deliberate).
- **F37** — `buddy_free` rejects order > MAX_ORDER before the merge
  loop can index past the free-list array.

## Why the per-finding severities are not recorded

The close commit attributes counts in aggregate (2 P1 + 4 P2 + 3 P3
fixed; 2 P3 deferred) across a scope wider than mm (kaslr, mmu,
boot), without per-finding severity tags. Rather than guess, the
Record keeps the aggregate on [[adt-p1id-r1]] and no per-finding
fnd notes were minted for this round.
