---
id: fnd-c9-r1-f3
type: fnd
round: adt-c9-r1
severity: P3
status: fixed
title: "`sched_record_space`'s DSB is unconditional by a comment alone"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c9-r1
regression: "none: no test can see a missing DSB"
created: 2026-11-04
---
## Prosecution

The DSB completes a stolen thread's EL0 cache maintenance on the CPU it left, and runs before both switch sites. Sound today. Nothing would notice someone making it conditional on the record changing, which is what an optimiser reading the same-space path would try next.

## Disposition

The Scheduler row's prosecution list names it unconditional by contract.
