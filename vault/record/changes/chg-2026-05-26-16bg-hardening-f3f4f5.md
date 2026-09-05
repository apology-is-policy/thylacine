---
id: chg-2026-05-26-16bg-hardening-f3f4f5
type: chg
title: "The memory-model hardening: the 8 GiB cap + the zero-visibility barrier"
date: 2026-05-26
arc: arc-pouch-boot
commits: ["e36197c9"]
touched: [sub-kernel-mm-phys]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

The P6 memory-model audit's follow-up close, mm slice: **F3** capped
the buddy zone at `mem_base + 8 GiB` so `KP_ZERO`'s direct-map
dereference can never run past `l1_directmap`'s reach (an unhandled
EL1 translation fault), and **F5** added the `dsb ish` after the
zeroing loop so a second CPU mapping the same PA via a different VA
sees zeroes. (F4, the pgtable-page zeroing + teardown ordering, is
mmu/proc surface.) The cap's relative-vs-absolute loose end surfaced
later at the #808 audit ([[seam-mm-directmap-cap-absolute]]).
