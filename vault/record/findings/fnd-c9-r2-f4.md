---
id: fnd-c9-r2-f4
type: fnd
round: adt-c9-r2
severity: P3
status: fixed
title: "Two stale comments on the surface this round read"
surface: [sub-kernel-thread]
threatens: []
fixed-by: chg-2026-11-05-c9-r2
regression: "none: comments"
created: 2026-11-05
---
## Prosecution

`kernel/main.c` says SP_EL0 is re-anchored where start.S has moved SP_EL1 since P5-el1h. `test_uaccess.c` says the fault is at L0 where the retired tables fault at L2.

## Disposition

Both corrected.
