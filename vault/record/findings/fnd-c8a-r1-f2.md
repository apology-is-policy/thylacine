---
id: fnd-c8a-r1-f2
type: fnd
round: adt-c8a-r1
severity: P2
status: fixed
title: "torpor.record_outlives_the_wake passed as a skip on one CPU"
surface: [sub-kernel-torpor]
threatens: []
fixed-by: chg-2026-11-03-c8a-r1
regression: "test_requires"
created: 2026-11-03
---
## Prosecution

The test needs a peer, and on one CPU it returned as if it had passed.

## Disposition

It says so through `test_requires`.
