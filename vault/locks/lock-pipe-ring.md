---
id: lock-pipe-ring
type: lock
title: "pipe_ring.lock — the ring's object lock"
kind: spin (non-irqsave)
guards: "count/head/tail, both EOF flags, and the atomicity of devpipe_poll's sample-with-register"
orders-before: [lock-poll-list]
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

One per ring, shared by both endpoints. Every mutation of ring state
happens under it; every wake happens AFTER it drops. The sequence on
each edge is fixed: mutate under lock → unlock → `wakeup` (the
opposite direction's rendez) → `poll_waiter_list_wake`. The sleep
side's cond (`cond_can_read`/`cond_can_write`) reads ring state
WITHOUT this lock — under the rendez lock instead — sound because
the producer's `wakeup` acquires that same rendez lock after its
mutation (the release/acquire pairing the rendez contract
documents).

`devpipe_poll` holds it across sample + register — the one place it
nests [[lock-poll-list]].

NOT the ring refcount's lock: `ref` is `__atomic` ACQ_REL
([[fnd-r15b-f234]]) precisely so two endpoint closes on two CPUs
need no shared lock for the last-one-frees decision.

## Held across

Byte copies into/out of the ring (bounded by PIPE_BUF_SIZE). Never
across a sleep or a wakeup.

## Prosecution

- Moving a `wakeup` inside the locked region deadlocks nothing today
  but widens the lock across the whole wait-chain acquisition — keep
  wakes after the drop.
- Any new ring-state field must be mutated under this lock AND
  covered by the cond-visibility argument (rendez-lock pairing), or
  it silently exits the [[spec-pipe]] envelope.
