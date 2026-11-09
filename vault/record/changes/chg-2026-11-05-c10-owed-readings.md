---
id: chg-2026-11-05-c10-owed-readings
type: chg
title: "C10's owed readings: a witness for the vector's tag-bit test, and two sabotages read alone"
date: 2026-11-05
arc: arc-phase6-pouch
commits: [2d4c38abe]
touched:
  - sub-kernel-exception
established: []
closed: []
opened: []
depth: skeletal
created: 2026-11-05
---
C10 landed with three readings not settled. `thread.kstack_fault_off_the_range_is_no_overflow` is new and gives the vector's tag-bit test a witness. With each test alone under HVF, a control green first: K02 reddens `thread.kstack_release_invalidates_a_warm_entry` on "stops translating", which settles `fnd-c10-r1-f3`'s item (e); K15 reddens `thread.kstack_cache_is_bounded` on "translates nowhere"; K10 ends the boot at the new test, "caught at the vector".
