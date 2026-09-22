---
id: fnd-b0poll-r6-s1
type: fnd
title: "Two masked pollers on one CPU hand it to each other through sched() inside the masked syscall, so a per-thread sleep bound never unmasks the CPU"
round: adt-b0poll-r6
severity: P1
status: fixed
surface: [sub-kernel-poll, sub-kernel-sched]
threatens: [inv-i9]
hazard: haz-latch-keyed-on-proxy
fixed-by: chg-2026-09-22-poll-preemption-point
regression: "specs/poll_cpu_buggy_sleep_only.cfg (CpuServesIrqs VIOLATED)"
created: 2026-09-22
---

## Prosecution

Round 5's backstop makes a poller really sleep once a millisecond has passed
with no real sleep, and a real sleep unmasks -- for THAT THREAD. Put two such
pollers on one CPU. Each hits its own budget, each really sleeps, and each
hands the CPU directly to the other through `sched()` called INSIDE the masked
syscall body. Neither thread returns to EL0, so neither unmasks, and the CPU
serves no interrupt for as long as the pair is fed. K = 1 is safe; K >= 2 is
not, and nothing in the mechanism distinguishes them.

A per-CPU backoff does not rescue it either. `g_timerwait` is global and
`timerwait_tick` runs on every CPU's tick, so another CPU expires the backoff
this one is counting on; with K pollers and a pass cost at or above the backoff
period the CPU stays saturated with masked passes. The general statement: a
sleep bounds the RATE of masked passes and never the masked SPAN.

## Disposition

FIXED by [[chg-2026-09-22-poll-preemption-point]]: the backstop is deleted and
every non-terminal pass crosses `sched_preempt_point`, whose bound is per-PASS
and therefore composes across pollers. Checked formally rather than by attack:
`specs/poll_cpu.tla`'s buggy cfg IS this finding -- two pollers on one CPU with
round 5's entire guarantee granted as fairness, `EachPollerSleeps` holding
(measured, the positive control) and `CpuServesIrqs` violated.
