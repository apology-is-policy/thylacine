---
id: seam-eevdf-math
type: seam
title: "The EEVDF math is unwritten, so I-17 is a target not a bound"
status: open
surface: [sub-kernel-sched]
opened-by: chg-2026-05-05-p2b-sched-dispatch
tracker: "ARCH 2A-F6 -> RW-13"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

The weighted virtual-time advance —
`vd_t = ve_t + slice × W_total / w_self` — and with it a real
[[inv-i17]]. What exists is a monotonic yield counter: on yield, `vd_t`
is stamped past everything currently queued, so a band rotates FIFO.
`Thread.weight` exists, defaults to 1, and **is read by nothing**.

Also owed alongside it: [[spec-scheduler]]'s per-thread fairness
refinement (a `Yield(cpu, t)` parameterized action), which is what would
let the model check a per-thread bound rather than the qualitative
"eventually runs".

## Why it was deferred

At P2-Ba the counter was FIFO-equivalent and sufficient, and "elapsed
virtual time" is only meaningful once tick preemption exists. Preemption
landed at P2-Bc; the math did not follow, and the gap has since been
papered over case-by-case — wake-preemption for the slice cliff, the
`vd_t` clamp for the cross-CPU stale key, the cross-CPU `need_resched`
for the placement leak. Each removed a specific unbounded-looking wait.
None established a bound.

## Cost of leaving it

The vocabulary claims more than the code delivers: bands, `vd_t`,
"EEVDF". Anyone reading the scheduler will assume a latency bound exists.
The honest position is recorded at [[inv-i17]] — and the risk is not
that latency is bad today (it is measured, and fine) but that a future
regression has no invariant to fail against.
