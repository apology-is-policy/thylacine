---
id: spec-sched-tickless
type: spec
title: "sched_tickless.tla"
models: [sub-kernel-sched-smp]
pins: [inv-i9, inv-i8]
cfgs:
  - "sched_tickless.cfg -- clean: NoLostWake + ParkedImpliesRegistered + RunningNotParked + the EventuallyRuns witness"
  - "sched_tickless_buggy.cfg -- BUGGY_PARK: park-before-register, the lost wake at depth 4"
gate: "any change to the idle park sequence, the one-shot arm, or the idle_in_wfi announcement"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

The **idle arm-race**, and only that. A CPU announcing itself idle,
arming a one-shot timer, and parking — against a peer placing work in the
window between those steps.

Written model-first for the tickless arc. The observation that made it a
*sibling* rather than an extension: [[spec-scheduler]] already proves the
tickless wake-correctness, because the periodic tick was never a modelled
wake source. A WFI'd CPU in that model is woken only by the work-arrival
IPI or a deadline, and `LatencyBound` holds without any tick at all. The
model was already tickless-shaped; the implementation was the laggard.

What it needed was the *split* of the atomic `EnterWFI` into a register
step and a park step, plus a new flag and a changed `NotifyWFIPeer`
precondition — which would ripple through `scheduler.tla`'s eleven cfgs.
The [[spec-sched-oncpu]] / [[spec-sched-alpha]] precedent applies: model
the one new mechanism in its own module and leave the audited one alone.

## What it pins

- `NoLostWake` / `ParkedImpliesRegistered` — the register-then-observe
  obligation: `idle_in_wfi` is set **before** the arm and the WFI, so a
  peer either sees the announcement and kicks, or sends an IPI that the
  WFI takes pending. This is [[inv-i9]] on the placement path.
- `RunningNotParked` — a CPU with work is not parked.
- `EventuallyRuns` — the liveness witness ([[inv-i8]]).

`BUGGY_PARK` is the counterexample: park before registering, and a flag
set in the window is lost, stranding the poller asleep on a ready CPU.

## Blind spot — recorded deliberately

**This module cannot express #363.** It has no self-requeue action, so a
CPU parking over its *own* just-requeued thread is outside its state
space, and the park-guard regression would pass the whole spec family.

That is acceptable for a latency-class bug bounded by the backstop, but
it is written here so the next change to the park-commit logic does not
mistake a green spec run for coverage. The #363 park-guard's deferred
park is a **stutter** on this module's Park action — `NoLostWake` and
`ParkedImpliesRegistered` are untouched by it, which is why the fix
needed no model change and also why the model would not have caught the
bug.

## Gate

Both cfgs on any change to the idle park sequence, the one-shot arm, or
the `idle_in_wfi` announcement. Read the blind spot above before
concluding a park-path change is covered.
