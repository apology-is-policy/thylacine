---
id: lock-rng-dev
type: lock
title: "g_rng_dev_lock — the entropy device guard"
kind: spin
orders-before: [lock-buddy-zone]
guards: "exclusive use of the host entropy device: its bring-up, its single queue, the page staged for it, and its teardown"
created: 2026-08-02
updated: 2026-08-02
---
## Discipline

Serializes the whole lifecycle of one pull from the host entropy device —
reset, negotiate, arm a queue, publish a buffer, poll for completion, reset
again, free. The device is brought fully up and fully back down inside a single
hold, so it is dormant between pulls and two concurrent re-seeds can never both
be driving it.

Held **across a page allocation and free**, which puts the buddy allocator
beneath it. That order is acyclic only because no interrupt handler takes this
lock; both callers today (the boot seed and the periodic top-up) are ordinary
process context. An interrupt-context consumer would have to be reconciled with
the bounded poll, which cannot be held with interrupts masked.

Never held together with [[lock-random]]. The pull completes and the lock is
released; the caller then takes the state lock separately to mix the result. See
that note for why the split is deliberate rather than incidental.

The poll inside the hold is bounded twice — by wall-clock time and by an
unconditional iteration ceiling — so the lock is never held indefinitely even
against a device that has stopped answering or a clock that has stopped
advancing. The worst case is one wall-clock budget per hold, which a second
caller waits out.
