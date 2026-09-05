---
id: lock-timerwait
type: lock
title: "g_timerwait.lock — the global deadlined-sleeper list lock"
kind: spin-irqsave (irqsave everywhere -- the timer IRQ takes it)
guards: "the single global list of threads inside a deadlined tsleep, threaded through Thread.timerwait_next/prev, plus each linked thread's sleep_deadline"
orders-before: [lock-rendez, lock-runq]
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

One global lock, not per-CPU, and that is a considered choice: a
deadlined wait is the cold path (a hung-server backstop, poll and futex
timeouts), the scan is O(timed sleepers) which is small, and the global
lock is what [[spec-tsleep]] verifies. Per-CPU sharding is a recorded
optimization ([[seam-timerwait-sharding]]), not a correctness need.

Position: below [[lock-wait]], above [[lock-rendez]].

    lock-proc-table -> lock-wait -> lock-timerwait -> lock-rendez -> lock-runq

**`wakeup` takes it as its outer lock even for a plain `sleep` waiter
that is never on the list.** It has to: it cannot know whether the waiter
is deadlined until it holds `r->lock`, and by then taking the global lock
would invert the order. So it takes both, unlinks if needed, and releases
the global one immediately — the `on_cpu` spin and the `ready()` that
follow run under `r->lock` alone. That release is not tidiness; keeping
the global lock across a spin that waits for a peer's context switch
would let one wakeup stall every CPU's tick.

**Always irqsave**, because the timer IRQ's `timerwait_tick` takes the
same lock. An IRQ landing mid-hold on the same CPU self-deadlocks.
`timerwait_earliest_deadline` — called from the idle path, which is
already masked — keeps the irqsave anyway so the helper is correct from
any context.

## Held across

- One unlink, in `wakeup`.
- One find-unlink-wake, in `timerwait_tick` — and then **released before
  the next**. The scan wakes expired sleepers one at a time, re-acquiring
  per iteration, so a burst of simultaneous timeouts cannot stall other
  CPUs behind a single long hold (the P5-tsleep F6 fix). Each individual
  wake is still atomic: both locks held continuously across it, so no
  re-enqueue window opens.
- The full three-lock registration in `tsleep`.

Never across `sched()`.

## Prosecution

- **`now` is sampled once per `timerwait_tick` pass**, so the set that
  pass wakes is fixed and the rescan-from-head terminates. Sampling per
  iteration could livelock the handler on a stream of newly-expiring
  sleepers.
- **The selected thread is unlinked unconditionally**, even on a re-check
  miss, so the rescan cannot re-select it and spin.
- **The `on_cpu` pre-filter stays.** An expired but mid-switch sleeper is
  skipped and caught by a later tick; without the filter the wake's
  `on_cpu` spin runs *inside the timer IRQ handler*, unbounded.
- **`timerwait_earliest_deadline` deliberately does NOT filter `on_cpu`**
  — it reads deadlines and wakes nothing, and a mid-switch sleeper's near
  deadline still needs covering by the one-shot. The asymmetry with
  `timerwait_tick` is intentional; making them agree would be a bug in
  one direction or the other.
- **A linked thread's `sleep_deadline` is never 0.** That is what makes
  the 0 return of `timerwait_earliest_deadline` an unambiguous "no
  deadline" sentinel for the tickless arm.
