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
  - "tsleep_buggy_double_wake.cfg -- NoDoubleWake violated (the tick wakes off a stale entry)"
  - "tsleep_buggy_owner_keeps.cfg -- NoStaleTimerEntry violated"
  - "tsleep_buggy_clear_first.cfg -- TimerEntryHasRendez violated"
  - "tsleep_buggy_unlink_early.cfg -- RendezOutlivesTick violated"
  - "tsleep_buggy_recheck_order.cfg -- TimeoutSound violated (deadline checked before cond)"
  - "tsleep_buggy_wedge.cfg -- the wait never terminates"
gate: "any change to the deadline path, the timer bases, or the order of the cond/timeout checks"
created: 2026-08-01
updated: 2026-11-03
---
## Abstraction

`sleep` has **two** wake sources serialized by one Rendez lock: the
condition becoming true, and `wakeup`. Its missed-wakeup freedom is
[[spec-scheduler]]'s. `tsleep` adds a **third** — the deadline, delivered
off the periodic tick from its CPU's timer base — so three actors now race for
one waiter, and this module is about that.

## What it pins

- `NoDoubleWake` — the waiter is woken exactly once, whichever source
  gets there first. The three-way race is the whole point.
- `NoStaleTimerEntry` — an entry exists only from the commit to the
  owner's leaving. A wake leaves it on the base, and the owner takes it
  off on its way out; the `owner_keeps` cfg is the counterexample.
- `TimerEntryHasRendez` — a linked entry always names its Rendez, since
  the tick reads it under the base's lock; the `clear_first` cfg clears
  `rendez_blocked_on` before the unlink.
- `RendezOutlivesTick` — `tsleep` never returns while the tick is on its
  way to or inside the Rendez's lock. An owner that reads its mark clear
  skips the base's lock, so the tick's unlink under `r->lock` is what
  holds this; the `unlink_early` cfg unlinks a step before it.
- `SleepingHasWaiter` — the Rendez and the thread agree.
- `WokenSound` / `TimeoutSound` — the return value is truthful. This is
  where the **cond-has-precedence** rule is proved: a wait satisfied at
  or past the deadline still reports AWOKEN, because `Commit` checks the
  condition first. The `recheck_order` cfg flips that and breaks
  `TimeoutSound`.
- `TsleepTerminates` — the liveness witness; the `wedge` cfg is the
  counterexample.

Together: [[inv-i9]] extended to three wake sources, plus the piece of
[[inv-i8]] that says a bounded wait ends.

## Impl correspondence

The model's `Commit` is `tsleep`'s loop head — cond first, then timeout,
then register. `Wakeup` and `TickFire`'s wake share one effect
(`wake_rendez_waiter`), which is why the impl can have one function for
both with a `timed_out` flag rather than two wake paths that could
disagree. The tick is split into `TickPick`, `TickLock` and `TickFire`
because the owner can read its mark without the base's lock, so the
tick's section is not atomic against it. `OwnerLeave` is the resume path.

Two impl properties sit *beneath* the abstraction and are prose-argued:

- `timerwait_tick` wakes **one at a time**, releasing the base's lock
  between wakes, so a burst of simultaneous timeouts cannot stall a
  sleeper leaving that base behind one long hold. The batching is a
  scheduling concern the model does not see.
- The `on_cpu` filter — an expired entry whose owner runs elsewhere or is
  still switching out is skipped and met by a later tick — keeps an
  unbounded spin out of the timer IRQ handler. The model has no `on_cpu`:
  `TickPick` may take any expired entry, a superset.
- One base is modelled. Each has its own lock and tick, so `Commit` is
  blocked by a tick holding the Rendez lock and not by one holding only the
  base's. The correct configuration never reaches that freedom: a tick in
  `"base"` means the entry is linked, so the owner has not left.
  `unlink_early` does.

## Gate

All nine cfgs on any change to the deadline path, the timer bases,
or the order of the cond/timeout checks. Note the ordering rule is the
easy one to "simplify" wrongly: checking the deadline first looks
equivalent and silently converts a satisfied wait into a spurious
timeout.
