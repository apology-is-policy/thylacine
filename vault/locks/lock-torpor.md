---
id: lock-torpor
type: lock
title: "A torpor bucket's lock"
kind: spin (non-irqsave)
guards: "one bucket's chain, its waiters' links, and every waker's write of their awoken"
orders-before: [lock-timerwait, lock-rendez]
created: 2026-08-01
updated: 2026-11-03
---
## Discipline

One per bucket, and they never nest: the serialization the [[inv-i9]] proof is stated over is
a word's own bucket. The sleeper's compare + register run under it; every wake walk (per-VA,
death, stop) takes it. The sleeper RELEASES it before `tsleep`, so only the wake side ever nests
it over [[lock-rendez]].

The #343 lock-free mismatch return and the R-5 pre-fault both exist
to keep traffic OFF this lock: the mismatch path never takes it, and
the pre-fault guarantees the under-lock user-VA load cannot
fault-and-SLEEP. The surviving fault edge, bucket lock → `vma_lock` → buddy, exists only in the
decommit-race window and is non-blocking (the lazy-anon arm).

## Held across

- WAKE's walk, and each mark's hold of [[lock-rendez]]. NOT `wakeup()`: that comes after the
  unlock, under `wake_held` ([[spec-torpor]]).
- The death and stop walks' `wakeup()`, one bucket at a time. This is what keeps a linked
  waiter's record alive for them, and both walks are rare.
- The under-lock `uaccess_load_u32` (post-pre-fault: resident or a
  clean -EFAULT or the non-blocking lazy re-fault).

## Prosecution

- `WAKE(addr, 0)` returns WITHOUT taking this lock — it is a literal
  no-op with no barrier semantics (torpor-8 F7); nothing may rely on
  it for ordering.
- The post-register die-pending re-check must stay under this lock —
  it is the register-vs-cascade-walk race's closure ([[inv-i24]]).
- Sharding per-bucket (the v1.x lift) must preserve ONE property
  above all: the death walk and a registering sleeper must still
  serialize on a common lock per bucket.
