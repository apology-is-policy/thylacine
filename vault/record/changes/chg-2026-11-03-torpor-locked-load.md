---
id: chg-2026-11-03-torpor-locked-load
type: chg
title: "A wait's load under its lock cannot sleep, whatever a peer's MAP_FIXED did"
date: 2026-11-03
arc: arc-phase6-pouch
commits: []
touched:
  - sub-kernel-torpor
  - sub-kernel-fault
established: []
closed: [seam-torpor-reclaim-uaccess]
opened: []
depth: skeletal
created: 2026-11-03
---
NOT LANDED, and nothing here has been built or booted. Found in C8a's self-audit, on the code C8a was rewriting; it dates from D-3. A peer's `mmap(MAP_FIXED)` over a futex word, landing between the wait's pre-fault and its load under the bucket lock, made that load reach the file page-in and sleep, which `sched()` answers with an extinction, from EL0. A fault taken with a counted lock held now refuses the page-in before it pins, and the wait reads the failed load as a spurious return. Regressions: `demand_page.file_miss_under_a_lock_refuses`, `torpor.word_replaced_under_the_wait`.
