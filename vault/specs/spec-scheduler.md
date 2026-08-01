---
id: spec-scheduler
type: spec
title: "scheduler.tla"
models: [sub-kernel-sched, sub-kernel-sched-smp, sub-kernel-rendez]
pins: [inv-i8, inv-i18, inv-i9]
cfgs:
  - "scheduler.cfg -- clean: the Invariants conjunction (3T x 2C)"
  - "scheduler_liveness.cfg / _wfi / _hwake -- LatencyBound + HardwareWakeProgress under fairness"
  - "scheduler_buggy.cfg -- BUGGY: the split cond-check/sleep counterexample (NoMissedWakeup, depth 4)"
  - "scheduler_buggy_steal.cfg -- BUGGY_STEAL: NoDoubleEnqueue violated"
  - "scheduler_buggy_ipi.cfg -- BUGGY_IPI_ORDER: IPIOrdering violated"
  - "scheduler_buggy_starve.cfg -- fairness dropped: LatencyBound stutters"
  - "scheduler_buggy_wfi.cfg / _hwake.cfg -- the WFI-wake counterexamples (R5-H F77/F78)"
gate: "any change to the wait/wake protocol, the IPI mechanism, or the run-queue state machine"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

The original scheduler model: a thread state machine over per-CPU
runqueues, the `WaitOnCond`/`WakeAll` wait/wake protocol, per-(src,dst)
IPI queues, and WFI as a park state that only an IPI or a wake lifts.

Its invariants are `StateConsistency`, `NoSimultaneousRun`,
`RunnableInQueue`, `SleepingNotInQueue`, `NoMissedWakeup`,
`NoDoubleEnqueue`, `IPIOrdering`, plus the `LatencyBound` and
`HardwareWakeProgress` temporal properties.

## What it pins

- **[[inv-i9]]** on the wait/wake path: `WaitOnCond` is atomic
  cond-check + sleep + enqueue under one lock, and the `BUGGY` cfg —
  which splits it into `BuggyCheck` then `BuggySleep` — produces the lost
  wakeup at depth 4. This is the model [[sub-kernel-rendez]]'s protocol
  maps onto directly.
- **[[inv-i18]]** via explicit per-(src,dst) queues.
- **[[inv-i8]]**'s qualitative form via `LatencyBound` under strong
  fairness on Resume/Yield/WakeAll.

## The blind spot, and why it matters

**`scheduler.tla` models `Steal` as a single atomic transfer between
runqueues, and has no `on_cpu` variable at all.**

That is not a small abstraction. The impl's entire difficulty is that
dispatch is *not* atomic: a thread is claimed, then its context is
loaded, and for a long time the boot CPU had a second dispatch route
independent of any runqueue. Every real SMP bug in the tree — #788, #806,
#860 and its smp8 sibling — lives in exactly the windows this model
smooths over. It proved the high-level state machine sound *under an
atomicity assumption the implementation does not satisfy*, and it stayed
green through all of them.

This is the concrete case for modelling the mechanism rather than the
intent, and it is why the redesign is gated by two additional modules
rather than by an extension of this one: [[spec-sched-oncpu]]
re-introduced the abstracted-away mechanism and reproduced #860, and
[[spec-sched-alpha]] is the model the fixed architecture is checked
against.

`scheduler.tla` is retained, unmodified, and still run — it remains
correct about what it does model, and its eleven cfgs are a real
regression net for the wait/wake and IPI layers. It is simply not the
authority on migration safety.

## Gate

Re-run the clean cfg plus all seven buggy cfgs on any change to the
wait/wake protocol, the IPI mechanism, or the runqueue state machine.
Note that a change to the *switch* protocol (claim, handoff, resume) is
gated by [[spec-sched-alpha]] instead — this module cannot see it.
