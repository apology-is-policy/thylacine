---
id: fnd-c8a-r1-f3
type: fnd
round: adt-c8a-r1
severity: P2
status: fixed
title: "The same test held a negative a never-run waiter satisfies, and a 200 ms race"
surface: [sub-kernel-torpor]
threatens: []
fixed-by: chg-2026-11-03-c8a-r1
regression: "torpor_held_spins_for_test"
created: 2026-11-03
---
## Prosecution

In the test phase `ready()` stays local and secondaries stay parked, so the wake it meant to race never ran where it meant to.

## Disposition

The waker is placed on a peer and kicked, with a far deadline and the cascade's wake as the second source, and the spin count is its positive control.
