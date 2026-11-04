---
id: chg-2026-11-03-c10b-directmap-blocks
type: chg
title: "The direct map keeps its blocks: the guard toggles and the boot page map are gone"
date: 2026-11-03
arc: arc-phase6-pouch
commits: []
touched:
  - sub-kernel-mmu
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-03
---
NOT LANDED, and nothing here has been built past the object files or booted. Once a stack's guard stopped being direct-map pages ([[chg-2026-11-03-c10a-kstack-range]]), `mmu_set_no_access_range` and `mmu_restore_normal_range` had no production caller, and `mmu_pagemap_directmap` had no reason: #808 page-mapped the allocator zone only so that those toggles need no runtime break-before-make. All three go, with the demoting walkers and the boot line that reported the tables. About 340 lines of `mmu.c`, 0.2% of RAM back, and the direct map's TLB reach back on real hardware. On TCG the reach costs nothing, so its gain is unmeasured there.

The operand's masking test moves to a stack page, which is now what that invalidate is for. `directmap.stays_in_blocks` replaces `directmap.pagemapped_808`: it allows the image's four L3 tables and no more, with a control each way.
