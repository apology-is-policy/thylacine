---
id: lock-larder-l-lock
type: lock
title: "larder.lock — the cache leaf"
kind: spin
orders-before: []
guards: "all three sub-cache arrays + hashes + the gen ring + the diagnostics counters of one p9_client's Larder"
created: 2026-07-31
updated: 2026-07-31
---
## Discipline

A dedicated NEAR-LEAF spinlock on `struct larder` (one per `p9_client`).
Every entry point is a single acquire/release pair — no cross-op held
state, no mid-op release. The ONLY lock ever taken below it is the buddy
zone lock via the non-blocking `kmalloc`/`kfree` (the lazy entry arrays,
the per-slot 4 KiB page buffers) — the established `l->lock → buddy`
order. It is NEVER held together with `c->lock` (an RPC that takes
`c->lock` runs entirely outside it — the #360 lock-across-sleep rule),
and never across a blocking op of any kind. Sleep-illegal under it.

The serves deliberately COPY OUT under the hold (≤ 88 B attr, ≤ 4 KiB
page, ≤ 128 KiB cached-open snapshot — bounded upstream) — the lock hold
IS the buffer-lifetime argument (an invalidate/evict/destroy cannot free
or rebind a buffer mid-copy). A pin-and-copy-outside-lock refinement must
bring a #847-style refcount with it. Process-context only (no IRQ path
takes it).
