---
id: seam-slub-debug-mode
type: seam
title: "SLUB has no double-free detection — no poison, no redzone, no cookie"
status: open
surface: [sub-kernel-mm-slub]
opened-by: chg-2026-05-04-p1e-slub
tracker: "named at P1-E; still unbuilt"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`kmem_cache_free` validates the page (PG_SLAB, cache backref, slot
boundary — F32) but not the OBJECT's state: a double free rewrites
the object's first 8 bytes with a stale freelist head, splitting the
list or minting a cycle, silently. The kernel-wide guard is
discipline ("one kfree per kmalloc") plus whatever downstream
corruption trips an extinction.

## The lift

A debug build mode: free-pattern poison (catches use-after-free
reads), a per-object cookie or a slab bitmap (catches double free at
the free site), optionally redzones (catches small overruns). Gate
behind a build flag so the hot path stays untouched. Becomes urgent
the first time an allocator-corruption hunt burns a day that a
poison pattern would have ended in a minute — the AEGIS-triplet
lesson pre-paid for this note.
