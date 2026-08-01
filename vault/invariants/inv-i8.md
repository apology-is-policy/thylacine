---
id: inv-i8
type: inv
title: "I-8 — every runnable thread eventually runs"
number: I-8
guards: [sub-kernel-sched, sub-kernel-sched-smp, sub-kernel-rendez]
validated-by: [spec-scheduler, spec-sched-alpha, spec-sched-rebalance, spec-sched-tickless, gate-smp]
strength: spec
created: 2026-08-01
updated: 2026-08-01
---
## Statement

A Thread that is RUNNABLE and queued somewhere eventually gets a CPU.
Work-conservation: no CPU sleeps while runnable work is queued and
reachable, and no queued thread is starved by the dispatch order.

This is the **qualitative** liveness property. Its quantitative
strengthening — a bounded *how long* — is [[inv-i17]], which is a design
target rather than an as-built guarantee.

## Enforcement

Four mechanisms compose, and the interesting history is that removing any
one of them has, at some point, produced a real starvation:

- **Dispatch order.** `pick_next` serves the highest-priority non-empty
  band; within a band the min-`vd_t` head, with the yield stamp pushing a
  yielder behind everything currently queued. That gives rotation within
  a band — but explicitly **not across** bands ([[sub-kernel-sched]]).
- **Preemption.** The 1 kHz tick ages the slice and requests a reschedule
  at expiry; `preempt_check_irq` consumes it at IRQ-return and at the
  syscall-return tail. The wake path adds a fourth producer so a
  higher-band wake does not wait a full slice.
- **Cross-CPU reach.** A runnable thread queued on a *busy* CPU must be
  reachable by an idle one. Pull is `try_steal` at idle entry; push is
  idle-preferring placement (TI-4b) and the busy-side overload kick
  (TI-4c). The kick exists because tickless idle silently removed the
  1 kHz re-poll that had been doing this job invisibly — a 2.4x boot
  slowdown ([[arc-tickless-idle]]).
- **The park guard.** A CPU must not park over its *own* queue. `sched()`
  picks before it requeues prev, so an idle can be dispatched with the
  preempted thread landing in the tree right after the pick, and no IPI
  exists for a local self-requeue (#363 — the re-check loop in
  `sched_idle_park`).

Two things must additionally be true for the reach half: an idle CPU must
have announced itself before parking (register-then-observe, the
[[inv-i9]] leg), and a cross-CPU placement must set the target's
`need_resched` — the *correctness* half, not gated on the notify flag —
so a busy target reconsiders the placed thread rather than running out
its slice (this was [[fnd-866-r1-f1]]).

## Validation

[[spec-scheduler]]'s `LatencyBound` is the temporal property, checked
with strong fairness on Resume/Yield/WakeAll; `scheduler_buggy_starve.cfg`
drops the fairness and produces the stuttering counterexample.
[[spec-sched-alpha]]'s `AlwaysRunning`/`IdleAvailable` carry the
redesign's shape (a CPU always has *something* to dispatch).
[[spec-sched-rebalance]] models the busy-side kick and its
register-then-observe obligation, with `buggy_nokick` and `buggy_nolift`
as the two ways to lose the work. [[spec-sched-tickless]]'s
`EventuallyRuns` covers the parked case. [[gate-smp]] is the empirical
backstop.

**blind-to:** none of the models express #363 — `sched_tickless.tla` has
no self-requeue action — so a park-over-own-queue regression would pass
the whole spec family. That is acceptable for a latency-class bug bounded
by the backstop, and it is recorded here precisely so the next change to
the park-commit logic remembers it. The runtime signal is the
work-conservation telemetry (`starved_ns` / `tickless_max_starved_ns`),
whose collapse from ~9.6 s to ~3.4 s per boot was the #363 fix's witness.
