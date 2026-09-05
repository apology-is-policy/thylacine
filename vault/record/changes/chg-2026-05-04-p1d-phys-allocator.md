---
id: chg-2026-05-04-p1d-phys-allocator
type: chg
title: "P1-D: the physical allocator — buddy + per-CPU magazines"
date: 2026-05-04
arc: arc-phase1-foundation
commits: ["198c48c8"]
touched: [sub-kernel-mm-phys]
established: [sub-kernel-mm-phys]
closed: []
opened: [seam-buddy-bulk-op]
mirrors-checked: []
depth: skeletal
---
## What

Buddy (Knuth split/merge, orders 0..18) + 16-entry per-CPU magazines
at orders 0/9 + the DTB-driven `phys_init` bootstrap. Born
single-CPU: `NCPUS = 1`, spinlock stubs, `my_cpu() == 0` — the shape
#807 would later have to make true. The refill/drain per-page buddy
locking was a day-one economy that became the named
[[seam-buddy-bulk-op]].
