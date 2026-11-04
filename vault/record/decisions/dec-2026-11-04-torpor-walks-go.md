---
id: dec-2026-11-04-torpor-walks-go
type: dec
title: "A futex waiter has no walk of its own: death and stop reach it as any sleep"
date: 2026-11-04
status: standing
decided-by: user-vote
affects: [sub-kernel-torpor, sub-kernel-death]
created: 2026-11-04
---
## Fork

`torpor_wake_all_for_proc` and `torpor_stop_wake_all_for_proc` walked every bucket on a death or a
stop, and WAIT re-checked the death predicate under the bucket's lock to close the race with the
death walk. All three predate the universal death-wake (#811). A waiter's private rendez is named by
`rendez_blocked_on`, so the per-peer cascade reaches it, and tsleep's register-then-observe closes the
same race. The interrupt-terminate wake has relied on exactly that since LS-5c.

## Decision

Operator vote 22: they go if three sabotages stay green as predicted: the death walk not reaching its
Proc, no die check after register, the stop walk waking nobody. A red on any one keeps them. Vote 29:
the first K08 inverted the walk's compare, so it also fabricated a completed wake for every other
Proc's waiters, and joey's marker `L31gzL` read a futex wait with no waker returning 0. The corrected
sabotage decides, with paired controls that must turn red.

## Consequences

C8a's second lease read the corrected death walk green and the die check green, with the controls that
remove the other path as well red at `proc.group_death_wakes_a_torpor_waiter`,
`proc.group_death_before_a_torpor_wait` and `proc.job_stop_preserves_torpor_wait`. A death no longer
takes every bucket's lock in turn, and no bucket's lock nests under `threads_lock`. A waiter's `proc`
field had no other reader and goes with them.
