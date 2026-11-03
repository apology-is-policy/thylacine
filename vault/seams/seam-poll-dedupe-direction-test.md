---
id: seam-poll-dedupe-direction-test
type: seam
title: "No test tells which hook poll_hook_once keeps when two objects share one list"
status: closed
surface: [sub-kernel-poll]
opened-by: fnd-pollwalk-r1-f2
tracker: "aux follow-up after the poll chunk"
created: 2026-10-28
updated: 2026-10-29
closed-by: chg-2026-10-29-thread-model-c7
---
## Owed

A test that fails if `poll_hook_once` keeps the LATER of two hooks on one list.
`poll.one_hook_per_list_per_call` polls one fd twice, so either survivor covers
it. The direction matters for two DIFFERENT objects on one list (a pipe's read
and write ends share the ring's list): a mutation of the first object after its
sample but before the second's registration reaches only the earlier hook.

## What closes it

A KERNEL_TESTS hook between two entries' scans in the poll core, mutating the
first entry's object there, with the second entry on the same list.

## Closed (2026-10-29, thread-model C7-0)

`poll.dedupe_keeps_the_earlier_hook` puts two different objects on one list and
mutates the first between its sample and the second's registration, through a
KERNEL_TESTS hook in the poll core: a poller whose dedupe kept the later hook
sleeps through that mutation. Run red first (the sabotage that keeps the later
hook fails it) and green on the tip.

## The risk while open

Low: the code keeps the earlier hook (`waiters[j]`, j < i), as its comment argues
and the audit re-derived; the gap is that a future edit inverting it would pass
the suite.
