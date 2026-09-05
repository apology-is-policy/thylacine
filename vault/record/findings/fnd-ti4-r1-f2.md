---
id: fnd-ti4-r1-f2
type: fnd
round: adt-ti4-r1
severity: P3
status: documented
title: "The ready() -> read-head window in the surplus test is preemptible"
surface: [sub-kernel-sched-smp]
threatens: []
created: 2026-08-01
---
## Prosecution

Between `ready(work)` and the read of the band head, the test is
preemptible by cpu0's 1 kHz tick. A slice expiry landing in that window
dispatches `work` — which unlinks it from the tree — so the head reads
NULL and the assertion sees a spurious `false`.

## Disposition

Documented, not fixed. The pattern is identical to the established one in
`scheduler.ready_on_cross_cpu_enqueue` and its siblings, and it is
empirically reliable: the window is a handful of instructions against a
1 ms tick.

The fix, where it has been applied elsewhere, is to mask the window —
which [[adt-33-r1]]'s F3 did for `yield_dispatches_queued_work`, using
exactly this pattern. It was not applied here because the assertion is
one of a family and changing one member makes the family inconsistent
rather than safer.

## Why a P3 rather than nothing

Because it is a **flake generator with a real cause**, and a flake with a
real cause is worth naming so that when it fires, nobody hunts a
scheduler bug. This is the same class as #857, where an "smp8 `cons.*`
flake" turned out to be a benign in-tree idle miscounted as work — the
cost of not knowing was days.
