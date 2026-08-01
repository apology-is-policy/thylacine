---
id: sub-kernel-mm-phys
type: sub
parent: moc-kernel-memory
title: "The page allocator — phys bootstrap, buddy, per-CPU magazines"
code: ["mm/phys.c", "mm/phys.h", "mm/buddy.c", "mm/buddy.h", "mm/magazines.c", "mm/magazines.h", "kernel/include/thylacine/page.h"]
audit: hard
guarded-by: []
validated-by: [gate-smp]
locks: [lock-buddy-zone]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

Every physical 4 KiB frame that is not the kernel image, the
struct-page array, the DTB blob, the initrd, or low firmware. Three
layers in one subsystem: `phys.c` is the DTB-driven bootstrap and the
public API (`alloc_pages`/`free_pages`/`kpage_alloc`), `buddy.c` is
the canonical Knuth split/merge machinery, `magazines.c` is the
per-CPU hot path in front of it. No numbered §28 invariant names this
layer; its obligations — allocation correctness and SMP soundness —
are the un-numbered floor everything above stands on, and the audit
table lists it as a trigger surface in its own right.

## Contract

- `alloc_pages(order, flags)` → `struct page *` or NULL (OOM /
  order > `MAX_ORDER` = 18). Magazine first (orders 0 and 9 only),
  buddy fallback. `KP_ZERO` zeroes via the direct map and then
  `dsb ish` (F5) so a second CPU mapping the same PA through a
  different VA sees zeroes. `KP_DMA`/`KP_NOWAIT`/`KP_COMPLETE` are
  accepted no-ops.
- `free_pages(p, order)` — magazine first (`mag_free` returns false
  for non-magazine orders), buddy fallback. NULL is a no-op.
- `kpage_alloc`/`kpage_free` — single-page convenience returning a
  **direct-map KVA** (P3-Bb; `pa_to_kva`/`kva_to_pa` round-trip).
- `phys_init()` — discover RAM, compute the FIVE reservations
  (low-firmware, kernel image, struct-page array, DTB, initrd),
  sort + verify disjoint (F29/F34 — a Pi-5-shaped DTB landing inside
  the ~96 MiB struct-page claim would otherwise be silently zeroed),
  free the gaps, init magazines, then **#808**: page-map the buddy
  direct map to L3 granularity while still single-CPU and IRQ-masked
  — after which runtime kstack-guard flips only toggle present L3
  leaves, never a break-before-make, eliminating the #806 IRQ-during-
  BBM race for the buddy zone by construction.
- Diagnostics: total/free/reserved pages, the #808 table-page cost,
  `phys_zone_bounds`.

## Mechanism

**The F3 cap**: `zone_end = min(DTB end, mem_base + 8 GiB)` — the
direct map (`l1_directmap[1..8]`) reaches PA [1 GiB, 9 GiB), and
`KP_ZERO`'s dereference of `pa_to_kva(pa)` past it is an unhandled
EL1 translation fault. The cap is **mem_base-relative** while the
direct map is **absolute** — they coincide only because QEMU virt has
`mem_base == 1 GiB`. That is the #808-audit F2 loose end
([[fnd-808-f2]], [[seam-mm-directmap-cap-absolute]]).

**Buddy** (`g_zone0`, single zone): sentinel-headed doubly-linked
free list per order 0..18; buddy of PFN p at order k is
`p ^ (1<<k)`; alloc pops the smallest sufficient order and splits
down (right buddies pushed back); free merges upward while the buddy
is `PG_FREE` at the same order, anchoring on the lower PFN.
`buddy_free` rejects `order > MAX_ORDER` with an extinction (F37 —
a corrupted `page->order` would walk past the free-list array).
`buddy_free_region` chops a range greedily at
`min(alignment, remaining, MAX_ORDER)`.

**Magazines** (#807, [[chg-2026-05-31-807-magazines]]): 16-entry
per-CPU stacks at orders 0 and 4 KiB/2 MiB, half-fill hysteresis
(refill/drain to 8). The fast path is IRQ-masked
(`spin_lock_irqsave(NULL)` — bare mask, no lock): the mask pins the
CPU identity across `my_cpu()` → pop/push and makes the set
non-reentrant (IRQ-context allocs can't re-enter; preemption is
IRQ-driven so no mid-op migration). Pre-#807 `NCPUS` was 1 — every
CPU shared one set and raced the non-atomic `count` RMW, the SMP
double-alloc. The count-corruption `ASSERT_OR_DIE` is the loud
regression guard. `my_cpu()` is `MPIDR_EL1.Aff0` clamped to range —
the kernel-wide dense-Aff0 assumption ([[fnd-807-f1]],
[[seam-sparse-mpidr]]: a clustered SoC folds two CPUs onto one slot
and reopens the shared-set race; the Lazarus canonical map is the
fix, uniformly with sched/gic/fault/halls).

Refill and drain call `buddy_alloc`/`buddy_free` **once per page** —
the buddy lock is taken 8 times per boundary crossing, not once per
batch. The bulk-op under one hold is the named lift
([[seam-buddy-bulk-op]]).

`magazines_drain_all` is **quiescent-only** — it walks peer CPUs'
sets with no coordination; test-harness and shutdown use only.

## Data structures

`struct page` — **48 bytes, `_Static_assert`-pinned** (P1-I F35;
the array scales with RAM — 24 MiB at 2 GiB — so silent growth is a
BSS tax): `next`/`prev` (free-list), `order`, `flags`
(`PG_FREE`/`PG_RESERVED`/`PG_KERNEL`/`PG_SLAB`), `refcount`, pad,
plus the two SLUB fields (`slab_freelist`, `slab_cache` — valid only
under `PG_SLAB`; a deliberate no-union choice).

**The refcount trap**, pinned in project memory and repeated here on
purpose: `page.refcount` LOOKS like the COW/BURROW share count and is
NOT — buddy and magazines set it 1 at alloc and 0 at free (an alloc
marker); SLUB repurposes it as the slab inuse count; the BURROW
refcount lives in `struct Burrow` (the #847 dual count), not here.
`page.h`'s own comment says "placeholder". A lineage/COW design that
reads it as a share count is wrong on arrival.

Magazine pages in the stack carry `flags = 0` — deliberately NOT
`PG_FREE` (magazine ownership is not free-list membership).

## Concurrency

`zone->lock` ([[lock-buddy-zone]]) is irqsave and a deep leaf — held
across split/merge only, never across anything that allocates or
sleeps. Order: bare-IRQ-mask → zone (magazines), kmem-cache →
zone (SLUB slab ops), `vma_lock` → zone (demand paging), larder leaf
→ buddy (page-buffer frees). The #808 boot page-map runs before
`smp_init` under a full mask, so its block→table demotes race
nothing.

## Invariants enforced

None numbered — deliberately recorded as such (the honest analog of
[[inv-i17]]'s prose strength). The nearest obligations: the F5
`dsb ish` underwrites the cross-CPU zero-visibility that the I-13
kernel/user info-leak arguments assume (no inv-i13 note exists yet —
that surface is unswept), and the #808 page-map is what makes the
kstack-guard path BBM-free. Validated by the boot smoke
(alloc/free/drain round-trip to an exact free count), the suite under
UBSan, and the multi-boot SMP gate.

## Error paths

`extinction`: DTB missing/no memory node, reservation outside RAM,
reservation overlap, order > MAX_ORDER at free, magazine count
corrupt. NULL: OOM at any order (caller policy — the allocator never
panics on OOM), order > MAX_ORDER at alloc.

## Performance

Hot path is a masked stack pop (~a dozen instructions); miss cost is
8 buddy-lock round-trips (the seam); `KP_ZERO` is the dominant cost
of a zeroed alloc (page-sized store loop + one `dsb ish`).

## Prosecution

- Any new caller of `pa_to_kva` on an allocator-returned PA is bound
  by the F3 cap's reach argument; raising the cap requires extending
  `l1_directmap` FIRST, and at any `mem_base != 1 GiB` bringup the
  cap must become absolute ([[seam-mm-directmap-cap-absolute]]).
- The magazine fast path's soundness is the MASK, not a lock — any
  path that touches `g_percpu` without it (or from a peer CPU, as
  `magazines_drain_all` does) must prove quiescence.
- `mag_alloc`/`mag_free` init page fields OUTSIDE the critical
  section — correct only while the page is private to the caller;
  a future shared-magazine design loses that.
- Anything reading `page.refcount` as a share count.

## Seams

[[seam-buddy-bulk-op]] · [[seam-mm-directmap-cap-absolute]] ·
[[seam-sparse-mpidr]] (the magazines leg).

## Caveats

- `docs/reference/06-allocator.md` (absorbed) was frozen at P1-D with
  one current #807 paragraph stitched in: `struct page` given as
  32 bytes throughout (and the array as 16 MiB with it), three
  reservations not five, no F3 cap, no #808, SLUB listed as "not yet
  implemented" — and its head claimed refill amortizes "each
  acquisition covers 8 pages" while the appended #807 paragraph
  correctly describes per-page acquisition: the assert-and-opposite
  shape, in the doc the magazines.c header explicitly corrects.
- `mm/phys.h`'s own file comment is frozen at P1-D ("kpage_alloc
  returns a void* that's a cast load PA — TTBR0 identity-maps…";
  "returns PA-as-void*") while phys.c documents the P3-Bb KVA
  correctly — header-vs-body partial update in code.
- `page.h`'s `KP_NOWAIT` comment still says "implicit at v1.0 (no
  scheduler)".

## Provenance

[[chg-2026-05-04-p1d-phys-allocator]] →
[[chg-2026-05-05-p1id-closing-audit]] (F29/F34/F37 among them) →
P3-Bb direct map / P3-Bda / P4-E initrd →
[[chg-2026-05-26-16bg-hardening-f3f4f5]] (the cap + the barriers) →
[[chg-2026-05-31-807-magazines]] →
[[chg-2026-05-31-808-directmap-pagemap]].
