---
id: spec-torpor
type: spec
title: "torpor.tla"
models: [sub-kernel-torpor]
pins: [inv-i9]
cfgs:
  - "torpor.cfg -- clean: a wake of one"
  - "torpor_broadcast.cfg -- clean: a wake of everyone, NoLostWake biting"
  - "torpor_buggy_no_wait.cfg -- NoUseAfterFree violated"
  - "torpor_buggy_lifo.cfg -- OldestFirst violated"
  - "torpor_buggy_count_any.cfg -- CountHonest violated"
  - "torpor_buggy_eintr_any.cfg -- CountHonest violated"
  - "torpor_buggy_unlocked_check.cfg -- NoLostWake violated"
  - "torpor_buggy_stop_at_mark.cfg -- NoStranded violated"
gate: "any change to the walk, the mark, the count, the wait for wake_held, or the compare-and-link"
created: 2026-11-03
updated: 2026-11-03
---
## Abstraction

One bucket. A waiter links under its lock, sleeps with the lock dropped, and takes the lock
again to leave. A wake marks under the lock and wakes after it, so its batch reaches records past
the lock, and each record's frame waits for it. A deadline and a caught note end a sleep from
outside, each asking the cond first, and a stop parks a waiter on no rendez with its record in the
chain, which is how a mark meets a waiter that is not asleep.

Not modelled: the death walk, which holds the lock across `wakeup` and so cannot lose a record;
more than one bucket; the hash.
