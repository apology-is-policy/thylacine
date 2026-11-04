---
id: fnd-c8a-r2-f1
type: fnd
round: adt-c8a-r2
severity: P2
status: fixed
title: "The model is green on the lost wake K06 produces: a mark between a waiter's read and its registering"
surface: [sub-kernel-torpor]
threatens: []
fixed-by: chg-2026-11-04-c8a-r2
regression: "specs/torpor_buggy_unlocked_mark.cfg, MarkedSleeperIsHeld"
created: 2026-11-04
---
## Prosecution

`Sleep` read `awoken` and went asleep in one step, so no cfg could express K06. Splitting it alone would still be green: `NoLostWake` read a mark as a wake, and `CountHonest` and `NoStranded` are vacuous there.

## Disposition

`SleepRead` and `SleepCommit` are the two ends of the rendez lock's hold, and `Reeval` folded into them. `MarkedSleeperIsHeld` is new and `NoLostWake` is tightened. The buggy cfg fails on the new invariant, and every other cfg still fails on its own.
