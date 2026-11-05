---
id: dec-2026-11-03-c8b-timer-bases
type: dec
title: "Per-CPU timer bases, and the sleeper unlinks"
date: 2026-11-03
status: standing
decided-by: user-vote
affects: [sub-kernel-rendez, sub-kernel-sched-smp, sub-kernel-sched]
created: 2026-11-03
---
## Fork

Every deadlined sleeper sat on one global list under one lock, and `wakeup` took that lock as its outer lock even for a plain `sleep` waiter that was never on the list, because it could not tell until it held `r->lock`. So every wake and every CPU's tick met on one lock.

Options: per-CPU sorted bases with the sleeper unlinking itself on every way out, so a wake takes no timer lock (Plan 9's `portclock.c`, Linux's hrtimer bases); a hash of per-Rendez timer state; or keep the global lock and shard only the tick.

## Choice

Per-CPU bases (operator vote 21, 2026-11-03). `tsleep` links on its current CPU's base; the sleeper unlinks itself; `wakeup` and the death and note wakers take no timer lock; `tsleep.tla`'s `NoStaleTimerEntry` is restated, since a wake now leaves an entry behind by design.
