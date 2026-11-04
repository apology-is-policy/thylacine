---
id: fnd-c8a-r1-f1
type: fnd
round: adt-c8a-r1
severity: P1
status: fixed
title: "A peer's MAP_FIXED between the wait's pre-fault and its locked load sleeps under the bucket's lock"
surface: [sub-kernel-torpor]
threatens: []
fixed-by: chg-2026-11-03-c8a-r1
regression: "demand_page.file_miss_under_a_lock_refuses, torpor.word_replaced_under_the_wait"
created: 2026-11-03
---
## Prosecution

A peer's mmap(MAP_FIXED) over the futex word, between the pre-fault and the load under the bucket's lock, makes that load a FILE miss. The page-in sleeps, and `sched()` extincts on a held lock.

## Disposition

The FILE miss refuses a faulter holding a counted lock, before the pin, and the wait reads the failed load as a spurious return. Also found by self-audit, at 4494357a1.
