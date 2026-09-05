---
id: fnd-rw1-af2
type: fnd
round: adt-rw1-mm-r1
severity: P2
status: fixed
title: "The global cache list had no lock — runtime create/destroy raced every walker"
surface: [sub-kernel-mm-slub]
threatens: []
fixed-by: chg-2026-06-10-rw1-allocator
regression: "the lock is structural; the SMP gate is the witness"
created: 2026-08-01
---
## Prosecution

`g_cache_list_head` + the `next_cache` chain were spliced with no
lock. Sound for BOOT (serial creates), silently unsound the moment
runtime create/destroy (the tests do both) or a diagnostic walker
(`slub_total_alloc` et al.) ran concurrently: a walker could read a
half-spliced head, or destroy's unlink could lose a concurrent
insert.

## Fix

`g_cache_list_lock` ([[lock-cache-list]]) — a strict leaf (never
held across a cache-lock acquire or an allocation; destroy unlinks
under it, frees the descriptor after release). Zero-init valid from
the first boot-time `init_cache`.
