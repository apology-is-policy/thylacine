---
id: lock-wait
type: lock
title: "Thread.wait_lock — the per-Thread outermost wait lock"
kind: spin-irqsave (carries the IRQ mask for the whole sleep, including across sched())
guards: "t->rendez_blocked_on, and the serialization of a sleeper's register-then-observe against a waker's read-and-wake of the same Thread"
orders-before: [lock-timerwait, lock-rendez]
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

One per Thread. The Plan 9 `p->rlock` analog, and the **outermost** lock
of the wait chain:

    lock-proc-table -> lock-wait -> lock-timerwait -> lock-rendez -> lock-runq

Its whole job is to make two critical sections mutually exclusive:

- **The sleeper's**, in `sleep`/`tsleep`: register on the Rendez (and the
  timer-wait list), transition to SLEEPING, then re-check the death and
  terminate predicates.
- **The waker's**, in the group-terminate cascade: read this Thread's
  `rendez_blocked_on` and `wakeup()` the Rendez it names.

Because they cannot interleave, every Thread either observes the flag in
its own re-check and never sleeps, or is found SLEEPING by the walk and
woken. There is no third case. That is [[inv-i9]]'s death-wake
generalization, and [[spec-death-wake]] is the proof.

It also **carries the IRQ mask** for the entire sleep — taken irqsave at
entry, restored at the very end — including across the `sched()` yields
in the middle. The inner locks are taken plain because the mask is
already held.

## Held across

**Not `sched()`.** Both locks are dropped before the yield and re-taken
on resume. A descheduled sleeper holding `wait_lock` would deadlock the
cascade — which needs that exact lock to wake it.

Held **across the `wakeup`** on the cascade's side, though, and that is
the opposite choice, made for a specific reason: `rendez_blocked_on` can
point into a sleeping peer's kernel stack frame, and holding the lock
across the wake is what pins the frame so it cannot be popped under the
waker.

Acyclicity rests on a single fact: **only the owning Thread ever writes
`rendez_blocked_on`**. A waker reads it; a waker never sets or clears it
(which is why `wake_rendez_waiter` deliberately does not).

## Prosecution

- **The re-check must happen after registration and under this lock.**
  Checking before registering, or checking outside the lock, is exactly
  `BUGGY_OBSERVE_BEFORE_REGISTER` in [[spec-death-wake]] — the
  non-reaping hang.
- **A hit must undo the FULL registration.** Rendez waiter, backref, and
  in `tsleep` the timer-wait link. Leaving the link strands an entry a
  later tick will wake into a thread that is no longer sleeping.
- **Nothing below this lock may take `lock-proc-table`.** The cascade
  holds the table lock and then this one; the reverse edge would close
  the cycle. `wait_pid` drops the table lock before it sleeps for this
  reason.
- **The stop detour re-acquires in order.** It drops all held locks,
  parks on the thread's own `debug_rendez`, and re-takes
  `wait_lock` → (`g_timerwait.lock`) → `r->lock` on resume. A shortcut
  that keeps one held across the park inverts the order.
