---
id: lock-poll-list
type: lock
title: "poll_waiter_list.lock — the per-object hook list"
kind: spin (non-irqsave)
guards: "the singly-linked poll_waiter hook chain, each hook's list backpointer, and the producer-side ready-flag writes during a wake walk"
orders-before: [lock-timerwait, lock-rendez]
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

Embedded in every pollable object (pipe ring, SrvConn, SrvService,
the cons layer, dev9p poll state) — internal to the list API:
register / unregister / wake / empty all take it themselves.

Position in the chain: **object lock → list lock → (wakeup's wait
chain)**. The register path holds the object's lock when it calls
`poll_waiter_list_register` — that outer hold is what makes
install-and-sample one atomic step (the register-then-observe of
[[spec-poll]]). The wake path runs AFTER the producer released the
object lock, walks under the list lock, sets each `ready` then
`wakeup(pw->rendez)` — the wakeup enters [[lock-timerwait]] →
[[lock-rendez]] while the list lock is still held.

**Unregister takes ONLY this lock.** That asymmetry is load-bearing:
the poll sweep runs with no object lock, so it can never deadlock
against a producer holding one.

Non-irqsave — no IRQ handler may enter it. The console's IRQ-side
readiness honors this by relaying to a kthread (the cons_poll
deferred wake); any new IRQ-context readiness source must do the
same, never widen this lock to irqsave.

## Held across

The wake walk including its per-hook `wakeup` calls (bounded by the
number of registered pollers). A stale magic mid-walk extincts —
that is the NoStaleHook tripwire firing.

## Prosecution

- Register extincts on double-register or bad magic; unregister on a
  set backpointer whose hook is absent from the chain. All three are
  corruption detectors, not error paths — do not soften them.
- The `ready`-before-`wakeup` write order inside the walk carries the
  flag through the rendez release/acquire pair; swapping them loses
  the flag for a cond that runs between.
