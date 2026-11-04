---
id: fnd-c8a-r2-f4
type: fnd
round: adt-c8a-r2
severity: P3
status: fixed
title: "A pinned thread's home is a dispatch stamp with no guard"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c8a-r2
regression: "sabotage V15a"
created: 2026-11-04
---
## Prosecution

An explicit placement re-homes a pinned thread for good, and nothing refuses it. No caller does that today.

## Disposition

`ready_on` extincts on a pinned thread that has run being placed off its `last_cpu`, and the comments the fix made false are corrected.
