---
id: fnd-c8a-r1-f4
type: fnd
round: adt-c8a-r1
severity: P3
status: fixed
title: "Readers of torpor_lock, and the ARCH order line skipped g_timerwait.lock"
surface: [sub-kernel-torpor]
threatens: []
fixed-by: chg-2026-11-03-c8a-r1
regression: "none: wording"
created: 2026-11-03
---
## Prosecution

Comments named a lock that no longer exists, and the order line left out a lock it passes through.

## Disposition

Corrected.
