---
id: fnd-c8a-r2-f5
type: fnd
round: adt-c8a-r2
severity: P3
status: fixed
title: "In the test phase the fix strands a pinned sleeper homed on a parked secondary"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c8a-r2
regression: "scheduler.pinned_sleeper_on_a_peer_wakes"
created: 2026-11-04
---
## Prosecution

A wake from cpu0 sets the peer's need_resched and sends a gated IPI, and the gate is shut, so a thread nobody can steal waits on a CPU with no tick. Every peer-pinned thread in the suite spins, so nothing showed it. Running on the waker's CPU had hidden it.

## Disposition

The IPI is not gated for a pinned thread, for which it is correctness and not promptness.
