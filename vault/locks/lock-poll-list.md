---
id: lock-poll-list
type: lock
title: "poll_waiter_list.lock — the per-object hook list"
kind: spin (irqsave since 2026-09-21)
guards: "the singly-linked poll_waiter hook chain, each hook's list backpointer, and the producer-side ready-flag writes during a wake walk"
orders-before: [lock-timerwait, lock-rendez]
created: 2026-08-01
updated: 2026-09-21
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

**Taken IRQSAVE by every operation** (B-0 audit round 4 F3,
2026-09-21). This note said "non-irqsave -- never widen this lock to
irqsave" until then, and the rule was half right. The half that
stands: **no IRQ handler walks a hook list** -- a walk is O(pollers)
nested wakeups, and the console's IRQ-side readiness is relayed to a
kthread so the per-byte IRQ stays O(1) (the cons_poll deferred wake);
a new IRQ-context readiness source relays the same way. The half that
was wrong: the lock NESTS UNDER object locks that IRQ handlers take
(`g_cons.lock`, `g_cons_drain.lock` -- the UART RX IRQ), and a lock
taken while holding an IRQ-taken lock must be held with IRQs masked
everywhere. Taken plain, console_mgr (a kthread, IRQs on) could be
interrupted mid-walk by an RX IRQ spinning on `g_cons.lock` while
another CPU held `g_cons.lock` in `cons_poll` spinning on this lock --
an ABBA through the IRQ edge that wedges the guest. That is lockdep's
"IRQ-unsafe lock nested under an IRQ-safe one"; Linux's wait-queue
lock is irqsave for the same reason. No deterministic test reaches
the interleaving; the guard is the comment at the list ops in
`kernel/poll.c`, the poll audit row, and the SMP gate.

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
- Every acquisition irqsave -- one plain `spin_lock` on a list that
  nests under an IRQ-taken object lock reopens the F3 wedge. And a NEW
  object lock that some IRQ takes, with a hook list under it, is this
  note's business before it is the Dev's.
- The contract of a wake is an ORDER, not a lock: the walk follows the
  readiness mutation's becoming visible under the lock the register +
  sample holds -- under that lock or after dropping it.
