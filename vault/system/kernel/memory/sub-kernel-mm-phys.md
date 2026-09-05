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
updated: 2026-08-16
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
(`PG_FREE`/`PG_RESERVED`/`PG_KERNEL`/`PG_SLAB`), `refcount`,
**`cow_share`**, plus the two SLUB fields (`slab_freelist`,
`slab_cache` — valid only under `PG_SLAB`; a deliberate no-union
choice).

**The pad is spent.** `cow_share` took the former 4-byte alignment
slack, so the struct is still 48 bytes and the per-RAM reservation did
not move — the static assertion would have failed the build otherwise,
and did not. The consequence is that **the next field here costs 8
bytes of struct and a proportional slice of BSS at every RAM size**;
there is no free slot left to absorb one quietly.

**The refcount trap**, pinned in project memory and repeated here on
purpose: `page.refcount` LOOKS like the COW/BURROW share count and is
NOT — buddy and magazines set it 1 at alloc and 0 at free (an alloc
marker); SLUB repurposes it as the slab inuse count; the BURROW
refcount lives in `struct Burrow` (the #847 dual count), not here.
`page.h`'s own comment says "placeholder". A lineage/COW design that
reads it as a share count is wrong on arrival.

**That prediction was tested and held.** The copy-on-write arc needed
exactly such a count and did not take `refcount`, on two *measured*
grounds rather than on the general warning: the buddy writes it **per
block head**, so every tail page of an order>0 block carries a stale
value; and SLUB double-books it as a slab inuse count observed reaching
85. Neither reaches the new field, because no allocator path touches
it.

**`cow_share` is ESTABLISHED, NEVER INHERITED, and that contract is the
whole of its correctness.** A page recycled through the buddy carries
whatever its last owner left, so a stale count is a premature free or a
leak. Every site that puts a page into an anon Burrow slot sets it to
one rather than assuming — a **closed, enumerated set of three** (lazy
populate, the demand-zero fault install, the break). Every other writer
of those slots is file-backed text, shared read-only and never broken,
and deliberately does not participate. The operations extinct on a zero
count rather than guess, because zero means a site skipped the
establish.

The allocator's own initialization zeroes it **for a clean initial state
only**. That is not maintenance: nothing in this layer keeps the field,
and nothing may read it on a page that is not currently in an anon
Burrow slot. The distinction matters because a field the allocator
*zeroes* looks like a field the allocator *owns*, which is precisely how
`refcount` became the trap above — **"a field whose name states a
contract nothing keeps."**

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
- **`cow_share` is not this layer's field.** It is zeroed at init for a
  clean start and otherwise untouched; no allocator path may read,
  write, or preserve it. A future allocator that "helpfully" maintains
  it recreates the refcount trap at the next field along.
- **A new `struct page` field is now a real BSS cost.** The slack is
  gone, so the next one grows the struct and the whole per-RAM array
  with it. The static assertion is the gate; do not raise it to make a
  field fit.

### The instrument trap: `phys_free_pages()` is blind to order-0

Worth its own heading because it produced a red that reads exactly like
a broken mechanism.

**An order-0 free does not reach the buddy at all.** `mag_free` pushes
it onto the current CPU's magazine, so the buddy's free count never
moves. A test that samples `phys_free_pages()` across an order-0
teardown therefore measures **zero delta whether the free happened or
not** — the instrument is blind to both arms equally, which is
indistinguishable from the code under test doing nothing.

That is the failure mode to fear: a blind instrument does not report
"cannot measure", it reports the number that means "broken". The copy-on-write
teardown test hit it, and the only reason a working mechanism did not get
"fixed" around it was a temporary diagnostic rather than reasoning from
theory.

`magazines_drain_all()` before each sample makes the figure a true total
again. Measured with the drain in place: the private teardown returns one
page, the shared teardown returns zero.

**The earlier draft of the same test was worse — it passed.** It asserted
on `PG_FREE`, which is unusable here because the buddy's coalesce anchors
on the **lower-pfn** buddy: a freed page that merges rightward never gets
the flag set on its own `struct page`, so the assertion succeeded
vacuously. A vacuous pass and a blind zero are the same defect at
different signs.

The surviving form asserts the **difference between two otherwise
identical runs**, so the incidental allocations on the path cancel
instead of having to be reasoned about individually.

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
[[chg-2026-05-31-808-directmap-pagemap]] →
[[chg-2026-08-16-page-cow-share]] (the pad spent, and the
order-0 instrument trap).
