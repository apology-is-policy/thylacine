# 07 — SLUB [ABSORBED INTO THE VAULT]

Absorbed at the memory/ipc-wake sweep (`chg-2026-08-01-mm-ipc-sweep`).
Its content now lives, code-verified and current, in:

    vault/system/kernel/memory/sub-kernel-mm-slub.md

(the embedded freelist, the meta-cache bootstrap, kmalloc's two
backends, F32/F33/F37, and the RW-1 guards.)

**What this file got WRONG by the time it was absorbed.** Entirely
frozen at P1-E (2026-05-04) — and it contains the partial-update
mechanism caught mid-act: the mechanism prose states "we don't track
full slabs separately — they re-add to partial on first free"
**twice**, while the struct-page listing's own field comment says
"free list / partial list / full list". The F33 author (P1-I-D, one
day later) updated the comment and left the prose. The struct
`kmem_cache` listing omits `full_list`/`nr_full` entirely.

Beyond that:

- `kmem_cache_destroy` described as silently leaking live objects,
  with "Debug mode (P1-I) will add an assertion" — RW-1 F-S1 made it
  a loud extinction via the complete `alloc_count - free_count`
  check; F-S2 documented the quiesce contract; F-S3 added the
  impossible-geometry create guard. None appear.
- `kfree` on a bad pointer: "UB (no validation). Phase 2 audit will
  tighten this" — F32 added slot-boundary and head-page validation
  the next day.
- The global cache list described lock-free; RW-1 A-F2 locked it.
- The kmalloc large-path failure analysis in the error table
  describes a mechanism (`ceil_log2` overflow) that is not the real
  hazard — the real one was the RW-1 A-F1 near-SIZE_MAX rounding
  wrap, absent from the doc.
- Every KVA/PA conversion shown predates the direct map.
