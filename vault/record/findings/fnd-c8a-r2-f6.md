---
id: fnd-c8a-r2-f6
type: fnd
round: adt-c8a-r2
severity: P3
status: fixed
title: "The cond's stall wrote a shared line on every wait, and an armed stall could leak"
surface: [sub-kernel-torpor]
threatens: []
fixed-by: chg-2026-11-04-c8a-r2
regression: "sabotage V17, TORPOR-KNOB"
created: 2026-11-04
---
## Prosecution

The exchange ran on every parking wait of a test image, and the test armed before creating its threads, so a failed create left the next waiter to stall one second masked with `g_timerwait.lock` held.

## Disposition

A load first, arming once both threads exist, and the runner releases a stall left armed and reddens the test that left it.
