---
id: lock-random
type: lock
title: "g_random_lock — the CSPRNG state guard"
kind: spin
orders-before: []
guards: "the cipher context, the keystream buffer, the count of bytes left in it, and the countdown to the next strong re-seed"
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

A single global spinlock over the random source's cipher state. It is a **pure
leaf**: nothing is taken beneath it, and every operation under it is arithmetic
on a fixed-size buffer.

That leaf property is not incidental — it is arranged. The expensive half of a
strong re-seed (bringing up the host device, allocating a page, spinning on a
completion) runs entirely **outside** this lock under [[lock-rng-dev]], and only
the absorb takes this one. The two are never held together, in either order, so
the allocator sits beneath the device lock and nothing sits beneath this one.

The decision to re-seed and the act of re-seeding are therefore split: a caller
reads the countdown under this lock, releases it, performs the device pull, and
re-takes it to mix and serve. Two callers can decide to re-seed concurrently;
both pulls are serialized by the device lock and both results are mixed, which
is harmless — mixing extra entropy is never wrong.

Process context only. An interrupt handler taking this lock would deadlock
against a preempted holder, and would additionally break the leaf arrangement
above, since the re-seed path it would join reaches the page allocator.

Two flags the lock also covers — whether the source is ready to serve, and
whether the host device has ever contributed — are additionally **atomic**, so
readers outside the lock can consult readiness without taking it. Readiness is
monotonic: it is only ever set, never cleared, so a lock-free read cannot observe
it going backwards.
