---
id: fnd-33-r2-f1
type: fnd
round: adt-363-r2
severity: P3
status: fixed
title: "The park-guard comment misattributed the no-lost-wake guarantee to a flag-gated IPI"
surface: [sub-kernel-sched-smp]
threatens: [inv-i9, inv-i8]
fixed-by: chg-2026-07-05-33-sys-yield
regression: "none -- the comment now names both notify paths and forbids the gate in words"
created: 2026-08-01
---
## Prosecution

The new #363 park-guard comment explained why a *peer's* concurrent
insert is not the race the guard closes, and grounded that on the
`idle_in_wfi` register-then-observe IPI.

Ground truth is different, and the difference matters:

- A peer that places **onto** this CPU IPIs **unconditionally** —
  `ready_on`'s cross-CPU tail does `need_resched_set` plus
  `sched_notify_cpu`, neither gated on `idle_in_wfi`.
- `idle_in_wfi` is read only by `select_idle_target` (push placement) and
  by the local-place `sched_notify_idle_peer`.

As written, the comment implied the cross-CPU placement IPI was
flag-gated — and therefore that gating it would be a legitimate
optimization. It would not: `idle_in_wfi` is a bare volatile store a peer
can read **stale-FALSE**, so gating the placement IPI on it means a
placed thread waits up to the backstop. That is precisely the class #363
had just closed, reintroduced through a different door.

## Fix

The comment now names both notify paths explicitly and **forbids the
gate** in words: *do not "optimize" that gate in*.

Also tightened alongside it: the claim that the park path is
"byte-identical to the pre-tickless periodic idle" — the park-guard runs
in *all* modes (dormant on pre-preempt secondaries; on cpu0's test phase
it only accelerates requeued-work service from ≤1 ms to immediate).

## The shape

A comment that describes a mechanism *more conservatively than it is*
reads as an invitation to relax it. This is the second instance in one
arc ([[fnd-33-r1-f2]] is the first), and both were caught by a round
prosecuting a fix rather than a feature — which is the argument for
recursing on a dirty close even when the counts look benign.
