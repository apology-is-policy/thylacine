---
id: lock-timerwait
type: lock
title: "A timer base's lock — one per CPU, over that CPU's deadlined sleepers"
kind: spin-irqsave (the tick's IRQ takes it; every other holder has IRQs masked)
guards: "one CPU's TimerBase: the deadline-sorted list threaded through Thread.timerwait_next/prev, its head, tail and earliest, and each linked thread's timer_base_cpu mark and sleep_deadline"
orders-before: [lock-rendez, lock-runq]
created: 2026-08-01
updated: 2026-11-03
---
## Discipline

One lock per CPU in `g_timer_base[]`, the shape of Plan 9's `portclock.c`
and Linux's hrtimer bases ([[dec-2026-11-03-c8b-timer-bases]]).

Position: below [[lock-wait]], above [[lock-rendez]].

    lock-proc-table -> lock-wait -> lock-timerwait -> lock-rendez -> lock-runq

The tick enters the chain here. **A wake does not take this lock at
all**: `wakeup` and the death and note wakers take [[lock-rendez]] alone,
so a wake leaves its sleeper's entry where it is, and the SLEEPER takes
it off on its way out of `tsleep`. That is what makes the entry's Rendez
valid under this lock: the owner cannot leave `tsleep`, and so cannot end
the Rendez's life, without passing through this lock or that Rendez's.

**At most one base's lock is held at a time.** A sleeper that migrated
leaves its old base (take, unlink, drop) before it takes its new one.

**A holder has IRQs masked**, because the tick's IRQ takes the lock of
the CPU it lands on. A sleeper links on the CPU it runs on, with IRQs
masked until it has switched out, so its own base's tick cannot find it
mid-switch. The exception is a secondary whose tick is not armed yet, the
deterministic test phase and the moment after it, which links on cpu0's
base and kicks cpu0.

`earliest` is written under this lock and read without it: the ordinary
tick and an idle CPU's one-shot take no lock at all. A stale read costs a
lock taken for nothing or, for a link from another CPU, the wait until that
link's kick lands: `gic_send_ipi` orders the store before the IPI. With the
kick gated off in the test phase, cpu0 reads it at its next tick, or an idle
cpu0 at its 4 ms backstop.

## Held across

- One link or one unlink: `tsleep`'s registration and undo arms,
  `timer_base_leave`.
- One find, unlink and wake, in `timerwait_tick`, and then **released
  before the next**, so a burst of simultaneous timeouts cannot stall a
  sleeper leaving this base behind one long hold (the P5-tsleep F6 fix).
- The full registration in `tsleep`, with [[lock-wait]] above and
  [[lock-rendez]] below.

Never across `sched()`.

## Prosecution

- **`timer_base_leave` precedes the clear of `rendez_blocked_on`.** The
  tick reads that field under this lock for as long as the entry is
  linked; the other order lets it find a linked entry with no Rendez,
  and it extincts ([[spec-tsleep]] `BUGGY_CLEAR_FIRST`).
- **The tick unlinks under `r->lock`, not before it.** An owner that
  reads its mark clear skips this lock and takes `r->lock` next, so that
  lock is all that stops it leaving `tsleep` while the tick still holds
  the Rendez's address ([[spec-tsleep]] `BUGGY_UNLINK_EARLY`).
- **The tick re-checks SLEEPING and `r->waiter` under `r->lock`.** A
  stale entry is normal here, not a fault: it must wake nothing
  (`BUGGY_TIMEOUT_STALE`).
- **Every way out of `tsleep` is off the base.** The caller owns the
  Rendez's life, and an extinction at the exit says so
  (`BUGGY_OWNER_KEEPS`).
- **`now` is sampled once per `timerwait_tick` pass**, so the set that
  pass wakes is fixed and the loop terminates.
- **The `on_cpu` filter stays.** An expired entry with `on_cpu` set is a
  stale one whose owner runs elsewhere, on its way to this lock, or a
  secondary's sleeper still switching out on cpu0's base. Selecting it
  would run the wake's `on_cpu` spin inside the timer IRQ handler.
- **A linked thread's `sleep_deadline` is never 0**, which is what makes
  an `earliest` of 0 an unambiguous "no deadline" for the tickless arm.
