---
id: lock-cache-list
type: lock
title: "g_cache_list_lock — the SLUB cache roster, a strict leaf"
kind: spin-irqsave
guards: "g_cache_list_head and every kmem_cache.next_cache splice/walk"
orders-before: []
created: 2026-08-01
updated: 2026-08-01
---
## Discipline

RW-1 A-F2 ([[fnd-rw1-af2]]). Boot-time cache creates are serial, but
runtime create/destroy and any diagnostic walker
(`slub_total_alloc` etc.) race the head-insertion splice without it.
Zero-init = unlocked, so it is valid from the first `init_cache`.

A **strict leaf**: never held across a cache-lock acquire or any
allocation. `kmem_cache_destroy` unlinks under it, releases, and only
then frees the descriptor (via the meta cache — which takes a cache
lock, hence the ordering).

## Held across

List walks only (the diagnostics sum counters cache-by-cache under
it — bounded by the cache count, no nested locks).

## Prosecution

- A future `/ctl/mem` walker that wants per-cache detail must copy
  under this lock and format outside it, or drop it before taking
  `c->lock` — no cache→list nesting exists today, so a list→cache
  nesting would mint the first half of an ABBA that any later
  "convenient" cache→list edge completes. Keep the leaf absolute.
