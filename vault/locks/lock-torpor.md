---
id: lock-torpor
type: lock
title: "torpor_lock — the global futex-bucket lock"
kind: spin (non-irqsave)
guards: "all 64 hash buckets, every waiter's bucket link, and every waiter's awoken flag"
orders-before: [lock-rendez]
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

ONE global lock for the whole wait-on-address surface — the
serialization the [[inv-i9]] prose proof is stated over: the
sleeper's compare + register run under it; every wake walk (per-VA,
death, stop) takes the same lock. The sleeper RELEASES it before
`tsleep`, so only the wake side ever nests it over
[[lock-rendez]] (inside `wakeup`).

The #343 lock-free mismatch return and the R-5 pre-fault both exist
to keep traffic OFF this lock: the mismatch path never takes it, and
the pre-fault guarantees the under-lock user-VA load cannot
fault-and-SLEEP (a blocking 9P page-in under a global spinlock was a
system-wide futex stall). The surviving fault edge —
`torpor_lock → vma_lock → buddy` — exists only in the
decommit-race window and is non-blocking (the lazy-anon arm).

## Held across

- The bucket walk INCLUDING each `wakeup()` — which can spin on the
  woken thread's `on_cpu` while a peer CPU switches it out. The
  documented serialization hazard ([[fnd-torpor8-r1-f2]],
  [[seam-torpor-lock-wake-spin]]); all three walks share it.
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
