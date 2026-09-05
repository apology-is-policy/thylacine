---
id: sub-kernel-mm-slub
type: sub
parent: moc-kernel-memory
title: "SLUB — the kernel object allocator"
code: ["mm/slub.c", "mm/slub.h"]
audit: hard
guarded-by: []
validated-by: [gate-smp]
locks: [lock-kmem-cache, lock-cache-list, lock-buddy-zone]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Variable-size kernel objects over single-page slabs with the freelist
embedded in the free objects themselves (zero per-free-object
metadata). `kmalloc-{8..2048}` power-of-two caches plus typed caches
(`kmem_cache_create`) for Proc/Thread/Spoor/Burrow/Handle-class
structs; requests above `SLUB_MAX_OBJECT_SIZE` (2048) bypass to
`alloc_pages` and `kfree` reads the order back from the page.

## Contract

- `kmalloc(n, flags)` — smallest fitting cache, or the large path for
  n > 2048 (returns a direct-map KVA). n == 0 ⇒ NULL. The
  **near-SIZE_MAX wrap guard** (RW-1 A-F1, [[fnd-rw1-af1]]): the
  page-rounding of an n within `PAGE_SIZE-1` of SIZE_MAX wrapped to a
  1-page success — which also defeated `kcalloc`'s n*size overflow
  check for size == 1 — now rejected before rounding.
- `kzalloc` = `| KP_ZERO`; `kcalloc` overflow-checks n*size.
- `kfree(p)` — NULL no-op; slab pages validate **slot-boundary**
  (F32: an interior pointer would corrupt the freelist or deref the
  wrong slab metadata); large pages validate pointer ==
  head-page PA (F32: an interior pointer lands on a tail struct page
  whose flags/order are post-split garbage). Both violations extinct.
- `kmem_cache_create(name, size, align, flags)` — NULL if size >
  2048 or if the align/size combination yields zero objects per slab
  (RW-1 F-S3 — pre-guard, an impossible geometry left an empty
  freelist that NULL-deref'd at first alloc). `KMEM_CACHE_PANIC_ON_FAIL`
  turns alloc OOM into extinction.
- `kmem_cache_destroy(c)` — **caller-quiesce contract** (RW-1 F-S2:
  no concurrent alloc/free on any CPU; the live-count read and the
  drain run without `c->lock`). A cache holding ANY live object
  extincts (RW-1 F-S1, [[fnd-rw1-fs1]]): `alloc_count - free_count`
  is the exact live count across full AND partial slabs — the
  pre-fix guard tested only `nr_full`, so a partial slab with a live
  object slipped through as a **silent free** (page recycled under
  the object → UAF) while the less-dangerous full-slab case was
  already loud.

## Mechanism

Alloc: pop the cache's first partial slab (or `slab_new` from the
buddy), pop its freelist head, bump the inuse count; a now-full slab
moves partial → full (**F33** — the explicit full list exists
precisely because untracked full slabs leaked at destroy). Free:
push onto the slab freelist; full → partial on the transition;
inuse == 0 ⇒ drain the page back to the buddy. Bootstrap: a static
`g_meta_cache` carries `struct kmem_cache` itself so
`kmem_cache_create` can allocate descriptors; the kmalloc caches are
static BSS.

## Data structures

`struct kmem_cache`: sizes, `slab_order` (0 at v1.0 — single-page
slabs only), `objects_per_slab`, partial + full sentinel lists with
counts, cumulative alloc/free/slab counters, `c->lock`, `next_cache`.
The slab page's `struct page` carries `PG_SLAB` + `slab_cache`
backref + `slab_freelist` + `refcount`-as-inuse. Free objects thread
the freelist through their own first 8 bytes (`SLUB_MIN_OBJECT_SIZE`
= 8 exists for exactly that pointer).

## Concurrency

Per-cache `c->lock` (irqsave; [[lock-kmem-cache]]) serializes
alloc/free including the nested `alloc_pages`/`free_pages` — so the
order kmem-cache → zone is a standing edge. The global cache list is
under `g_cache_list_lock` (RW-1 A-F2, [[fnd-rw1-af2]],
[[lock-cache-list]]): boot creates are serial, but runtime
create/destroy and any diagnostic walker race the splice without it.
It is a strict LEAF — never held across a cache-lock acquire or an
allocation; destroy unlinks under it, then frees the descriptor
after release.

## Invariants enforced

None numbered (same posture as [[sub-kernel-mm-phys]]). The audit
row's obligations: freelist integrity, partial/full membership
(`0 < inuse < capacity` ⇔ partial), no-live-object destroy.

## Error paths

NULL: OOM, oversize, impossible geometry. Extinction: wrong-cache
free, interior pointer (both paths), `PG_SLAB` with no backref,
live-object destroy, PANIC_ON_FAIL OOM.

## Performance

Cache-hit alloc/free is a lock pair + a pointer pop/push. The
diagnostics (`slub_total_alloc/free`, `slub_active_slabs`,
`kmem_cache_live_count`) walk under the list lock; live-count is a
tolerated-stale lock-free read pair.

## Prosecution

- The destroy guard must stay `alloc_count - free_count` — reverting
  to any per-list count re-opens F-S1's silent-free arm.
- The create-time geometry pre-check must mirror `init_cache`'s
  sizing exactly (min-align floor, min-object floor, round-up) or
  the rejection drifts from the reality it guards.
- No double-free detection exists — a double `kmem_cache_free`
  corrupts the freelist undetected ([[seam-slub-debug-mode]]). The
  audit discipline "one kfree per kmalloc" is the only guard.
- The list lock's leaf-ness: any future walker that needs per-cache
  state must drop the list lock before taking `c->lock`.

## Seams

[[seam-slub-debug-mode]] (poison/redzone/double-free cookie — named
at P1-E, still unbuilt) · multi-page slabs (`slab_order > 0`) for
>2 KiB typed caches — rejected today, mechanical when needed.

## Caveats

- `docs/reference/07-slub.md` (absorbed) taught "we don't track full
  slabs separately" twice in mechanism prose while its own
  struct-page field comment says "free list / partial list / full
  list" — the F33 author updated the comment and left the prose: the
  partial-update mechanism caught mid-act. It also described destroy
  as silently-leaking (F-S1 made it loud), kfree as "no validation"
  (F32 added two), the cache list as lock-free (A-F2), and all its
  KVA/PA conversions pre-date the direct map.

## Provenance

[[chg-2026-05-04-p1e-slub]] →
[[chg-2026-05-05-p1id-closing-audit]] (F32/F33 among them) → P3-Bb
KVA conversions → Phase-4 renames →
[[chg-2026-06-10-rw1-allocator]] (A-F1/A-F2/F-S1/F-S2/F-S3).
