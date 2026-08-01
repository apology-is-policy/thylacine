---
id: fnd-torpor8-r1-f1
type: fnd
round: adt-torpor8-r1
severity: P2
status: fixed
title: "WAKE counted matched-and-marked waiters, not actual wakeups"
surface: [sub-kernel-torpor]
threatens: [inv-i9]
fixed-by: chg-2026-05-23-torpor
regression: "torpor.wake_two_waiters_count_bound (also the F9 multi-waiter walk)"
created: 2026-08-01
---
## Prosecution

The bucket walk bumped the return count for every `(p, VA)` match it
set `awoken = 1` on. A waiter whose tsleep had already lapsed on its
own deadline (the timer wake ran first; `r->waiter` already NULL) was
still counted — the caller heard "1 delivered" while the consumer
returned ETIMEDOUT. In the timeout-vs-wake race the two user-visible
signals disagreed about whether a wake landed.

## Fix

`if (wakeup(&w->rendez)) woken++` — the count is `wakeup()`'s truth
(SLEEPING→RUNNABLE transitions). The `awoken` flag is still set so a
consumer that has not yet re-evaluated absorbs the wake; the pthread
re-check discipline covers the residue. This is case (c) of the
header's prose proof — added to the proof by the same round's F3.
