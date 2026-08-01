---
id: spec-tsleep
type: spec
title: "tsleep.tla"
models: [sub-kernel-rendez]
pins: [inv-i9, inv-i8]
cfgs:
  - "tsleep.cfg -- clean: the Invariants conjunction"
  - "tsleep_nodeadline.cfg -- the deadline_ns == 0 degradation to plain sleep"
  - "tsleep_liveness.cfg -- TsleepTerminates"
  - "tsleep_buggy_double_wake.cfg -- NoDoubleWake violated"
  - "tsleep_buggy_lazy_unlink.cfg -- NoStaleTimerEntry violated"
  - "tsleep_buggy_recheck_order.cfg -- WokenSound violated (deadline checked before cond)"
  - "tsleep_buggy_wedge.cfg -- the wait never terminates"
gate: "any change to the deadline path, the timer-wait list, or the order of the cond/timeout checks"
created: 2026-08-01
updated: 2026-08-01
---
## Abstraction

`sleep` has **two** wake sources serialized by one Rendez lock: the
condition becoming true, and `wakeup`. Its missed-wakeup freedom is
[[spec-scheduler]]'s. `tsleep` adds a **third** — the deadline, delivered
off the periodic tick from a global list — so three actors now race for
one waiter, and this module is about that.

## What it pins

- `NoDoubleWake` — the waiter is woken exactly once, whichever source
  gets there first. The three-way race is the whole point.
- `NoStaleTimerEntry` — a woken or timed-out thread is off the timer-wait
  list. The `lazy_unlink` cfg is the counterexample: leave the entry and
  a later tick wakes a thread that is no longer sleeping.
- `SleepingHasWaiter` — the Rendez and the thread agree.
- `WokenSound` / `TimeoutSound` — the return value is truthful. This is
  where the **cond-has-precedence** rule is proved: a wait satisfied at
  or past the deadline still reports AWOKEN, because `Commit` checks the
  condition first. The `recheck_order` cfg flips that and breaks
  `WokenSound`.
- `TsleepTerminates` — the liveness witness; the `wedge` cfg is the
  counterexample.

Together: [[inv-i9]] extended to three wake sources, plus the piece of
[[inv-i8]] that says a bounded wait ends.

## Impl correspondence

The model's `Commit` is `tsleep`'s loop head — cond first, then timeout,
then register. `Wakeup` and `Timeout` share one effect
(`wake_rendez_waiter`), which is why the impl can have one function for
both with a `timed_out` flag rather than two wake paths that could
disagree.

Two impl properties sit *beneath* the abstraction and are prose-argued:

- `timerwait_tick` wakes **one at a time**, releasing the global lock
  between wakes, so a burst of simultaneous timeouts cannot stall other
  CPUs' ticks behind one long hold. Each individual wake is still atomic
  (both locks held continuously across it), which is the property the
  model needs; the batching is a scheduling concern the model does not
  see.
- The `on_cpu` pre-filter — an expired but mid-switch sleeper is skipped
  and caught by a later tick — keeps an unbounded spin out of the timer
  IRQ handler. The model has no `on_cpu`.

## Gate

All seven cfgs on any change to the deadline path, the timer-wait list,
or the order of the cond/timeout checks. Note the ordering rule is the
easy one to "simplify" wrongly: checking the deadline first looks
equivalent and silently converts a satisfied wait into a spurious
timeout.
