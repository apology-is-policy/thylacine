---
id: fnd-c8a-r2-f2
type: fnd
round: adt-c8a-r2
severity: P2
status: fixed
title: "scheduler.pinned_wake_goes_home could not reach its own assert when the mechanism broke"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c8a-r2
regression: "sabotage V15b"
created: 2026-11-04
---
## Prosecution

Under K15 the boot thread sits runnable on a peer whose waker spins on a `yield` instruction, so it never runs, and neither the assert nor the runner's check executes. The red was the unit leg and a 300 s timeout.

## Disposition

The waker calls `sched()` while it waits, so a misplaced boot thread is dispatched and the test reads where it is.
