---
id: seam-mm-directmap-cap-absolute
type: seam
title: "The 8 GiB RAM cap is mem_base-relative; the direct map is absolute"
status: open
surface: [sub-kernel-mm-phys]
opened-by: chg-2026-05-31-808-directmap-pagemap
tracker: "#808 audit F2; Lazarus board bringup"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

The F3 cap is `zone_end = mem_base + 8 GiB`, but the direct map
covers absolute PA [1 GiB, 9 GiB) — they coincide only while
`mem_base == 1 GiB` (QEMU virt). A board reporting a different
`mem_base` would let the buddy hand out PAs the direct map cannot
reach, and `KP_ZERO`'s dereference faults — the original F3 class,
reopened by geometry. `mmu_pagemap_directmap`'s own gib>8 skip is
already absolute-correct; only the cap is the loose end.

Dormant on every current target ([[fnd-808-f2]]).

## The lift

At any `mem_base != 1 GiB` bringup: express the cap absolutely —
`min(mem_base + 8 GiB, 9 GiB)` — or widen `l1_directmap` first. A
one-line fix whose entire cost is remembering it exists; that is
what this note is for.
