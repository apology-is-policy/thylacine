---
id: lock-rendez
type: lock
title: "Rendez.lock — the per-wait-object lock"
kind: spin-irqsave (irqsave at the wakeup entry; nested under wait_lock on the sleeper side)
guards: "r->waiter, and the atomicity of {evaluate cond, register the sleeper, transition to SLEEPING} against {make cond true, clear the waiter, transition to RUNNABLE}"
orders-before: [lock-runq]
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

One per `struct Rendez`, embedded freely in whatever object owns a wait.
It is the lock the whole missed-wakeup argument turns on: the condition
is evaluated under it, and the sleeper registers and transitions to
SLEEPING **before** it is released — so any `wakeup` after that release
sees the waiter, and any `wakeup` before the check has already made the
condition true.

Position in the chain: below [[lock-timerwait]], above [[lock-runq]].

    lock-proc-table -> lock-wait -> lock-timerwait -> lock-rendez -> lock-runq

**The producer's obligation is part of the lock's contract.** `cond`
reads the producer's state, so the producer must either hold `r->lock`
while making the condition true, or call `wakeup(r)` — which takes it —
afterward. A producer that does neither has no happens-before edge to the
consumer's `cond`, and the lock cannot help it.

## Held across

- The `on_cpu` spin in `wake_rendez_waiter` and the `ready()` that
  follows — deliberately, and deliberately **without**
  [[lock-timerwait]], which `wakeup` releases the moment its unlink is
  done. Keeping the global lock across a spin that waits for a peer's
  context switch would let one wakeup stall every CPU's tick.
- Never across `sched()`. The sleeper drops it (and `wait_lock`) before
  yielding.

## Prosecution

- **The unconditional acquire in `wakeup` is LOAD-BEARING, including on
  the no-waiter path.** It is the only ordering chain that delivers a
  torpor poster's `awoken = 1` — written before the call — to a
  stop-parked waiter's resumed `tsleep` re-loop, whose `cond` read pairs
  with this release. A lockless `r->waiter == NULL` fast path here reads
  as free performance and reintroduces a lost wake on the preserved-wait
  path (PTY-4e R2). If one change to this area is going to be made
  wrongly, it is this one.
- **Single waiter is enforced, not assumed.** A second sleeper on one
  Rendez extincts. Reaching that from EL0 would be an unprivileged panic,
  so any path that could see two threads on one wait needs a
  `poll_waiter_list` one layer up.
- **`wake_rendez_waiter` must not clear `rendez_blocked_on`.** That field
  is cleared only by its owning Thread, on resume, under
  [[lock-wait]] — clearing it here (under the wrong lock) races the
  death cascade's read (#811).
- **The three waker assertions stay**: the waiter is intact, is SLEEPING,
  and agrees with the Rendez about which Rendez it sleeps on. They are
  the only structural check that the wait state has not been corrupted by
  a third party.
