---
id: fnd-b0poll-r7-f3
type: fnd
title: "Both point tests stay GREEN with sched_preempt_point's body replaced by `return;` -- the test pollers are kthreads, so the unmask is inert"
round: adt-b0poll-r7
severity: P2
status: fixed
surface: [sub-kernel-poll, sub-kernel-sched]
threatens: [inv-i9]
hazard: haz-harness-fail-open
fixed-by: chg-2026-09-22-poll-preemption-point
regression: "sched.preempt_point_takes_a_pending_irq"
created: 2026-09-22
---

## Prosecution

`poll.point_services_noise` counts passes and samples; the counter sits one
line above the call, so it measures that the LOOP REACHED the point. The test
pollers are kernel threads, which already run with interrupts unmasked
(`thread_trampoline` does `msr daifclr, #2`), so the point's unmask is INERT in
every test. Replacing the whole body with `return;` leaves both point tests
GREEN. The mechanism the fix is named for was untested.

Matches self-found S3, reached independently in the parallel self-audit.

## Disposition

FIXED by `sched.preempt_point_takes_a_pending_irq`: mask with
`spin_lock_irqsave(NULL)` (the mask-only form, which does not touch
`preempt_count`, so the point's lock precondition still holds), busy-wait 3 ms
so this CPU's tick is certainly pending, assert `gic_cpu_irq_count(cpu)` has
NOT moved -- the control, without which the test would be satisfied by a CPU
that was never masked -- then cross the point and assert it HAS. Sabotages
`nopoint` (1612/1614, both poll point tests) and `nodaifclr` (1614/1615, the
witness at its own assertion) confirm the discrimination; `noisb` does not, and
that is [[fnd-b0poll-r7-s4]].
