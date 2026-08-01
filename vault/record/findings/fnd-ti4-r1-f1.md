---
id: fnd-ti4-r1-f1
type: fnd
round: adt-ti4-r1
severity: P3
status: documented
title: "The surplus test's baseline assertion assumes no NORMAL kthread is queued at test entry"
surface: [sub-kernel-sched-smp]
threatens: []
created: 2026-08-01
---
## Prosecution

`scheduler.cpu_surplus_for_kick` asserts `false` as its baseline — i.e.
that cpu0 holds no queued non-idle work — before readying its own thread
and asserting `true`.

That assumes no NORMAL or INTERACTIVE kernel thread (`console_mgr`, the
dev9p poll pump) is RUNNABLE-queued on cpu0 at the moment the test runs.
In practice they are all blocked on a rendez — SLEEPING, off-tree — by
the time the test phase runs, and the suite is green. But the assumption
is not enforced by anything.

## Disposition

Documented, not fixed. It is **shared with every sibling `sched.*`
test** and was not introduced by TI-4; fixing it in one place would be
misleading.

The hardening, if it is ever done, is to assert
`sched_runnable_count_band(NORMAL) == 0` first — turning an implicit
assumption into an explicit precondition — or to assert only the
*delta* across the `ready()` rather than an absolute baseline.

Worth noting the shape: a test whose baseline is "nothing else is
happening" is not testing the thing it names when something else *is*
happening. It fails for a reason unrelated to its subject, which is the
most expensive kind of test failure to diagnose.
