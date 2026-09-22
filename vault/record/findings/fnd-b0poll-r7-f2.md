---
id: fnd-b0poll-r7-f2
type: fnd
title: "IrqLatencyBounded is entailed by the very behaviour its own comment calls insufficient, so poll.tla never checked the CPU-level claim"
round: adt-b0poll-r7
severity: P2
status: fixed
surface: [spec-poll, spec-poll-cpu]
threatens: [inv-i9]
hazard: haz-harness-fail-open
fixed-by: chg-2026-09-22-poll-preemption-point
regression: "specs/poll_cpu_buggy_sleep_only.cfg"
created: 2026-09-22
---

## Prosecution

The property written for the point was

    IrqLatencyBounded == []<>(pc \in RealSleep \cup {"atpoint"} \cup Terminal)

which is IMPLIED by `[]<>(pc \in RealSleep)` by set inclusion. So it is
satisfied, in full, by round 5's sleep backstop -- the exact behaviour the
property's own comment calls insufficient -- and `poll_buggy_no_point`
re-proves round-5 F1 (a poller that never sleeps at all), never
[[fnd-b0poll-r6-s1]]. Measured by the prosecutor on scratch copies.

The defect is structural rather than textual: **a single-poller model cannot
carry a composition claim**, because what fails is what two pollers do to one
CPU.

## Disposition

FIXED by a NEW MODULE, `specs/poll_cpu.tla`: K pollers, one CPU, with the
`SleepHandoff` step that IS S1 and round 5's entire guarantee granted as
fairness (`SF_vars(SleepStep(p))`). `poll_cpu_buggy_sleep_only` VIOLATES
`CpuServesIrqs`; `poll_cpu_sleep_bound_holds` shows `EachPollerSleeps` HOLDS in
that same configuration (the positive control, one variable away), and
`poll_cpu_one_poller` shows K = 1 was sound. `check-poll.sh` is 16/16.

The module's first cut was itself wrong: it let a poller sleep straight out of
`run`, so the adversary could hand the CPU off before the point ever fired and
the CLEAN cfg failed. The `armed` state enforces the loop's real order -- a
pass reaches its point before it can sleep again.
