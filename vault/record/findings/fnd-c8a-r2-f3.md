---
id: fnd-c8a-r2-f3
type: fnd
round: adt-c8a-r2
severity: P3
status: fixed
title: "The runner's off-CPU check could redden the wrong test"
surface: [sub-kernel-sched-smp]
threatens: []
fixed-by: chg-2026-11-04-c8a-r2
regression: "none: a runner"
created: 2026-11-04
---
## Prosecution

A test that moved the boot thread and failed on an assert of its own printed only that, and the next test was reddened for the move.

## Disposition

An `OFF-CPU0` marker prints either way, and only the first test to return off CPU 0 answers for it.
