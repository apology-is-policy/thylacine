---
id: sub-kernel-pagemap
type: sub
title: "The pagemap -- a Burrow's charged, on-touch sparse slot table"
parent: moc-kernel-memory
code: [kernel/pagemap.c, kernel/include/thylacine/pagemap.h, kernel/test/test_capacity.c]
audit: hard
guarded-by: [inv-i32, inv-i7, inv-i44]
validated-by: [spec-capacity, gate-smp]
locks: [lock-burrow]
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md"]
created: 2026-09-23
updated: 2026-09-23
---
## Purpose

The per-page slot table of the two SPARSE Burrow types, `BURROW_TYPE_FILE` and
`BURROW_TYPE_ANON_LAZY` ([[sub-kernel-burrow]]): which slots of a reservation
hold a physical page. It replaced the flat `struct page *filepages[]` array --
eight uncharged kernel bytes per RESERVED page, allocated whole at create --
which was what pinned every lazy reservation to a byte cap
(`BURROW_RESERVE_MAX` at 1 GiB, and the eager `BURROW_ATTACH_MAX` of 256 MiB
as the detach's own bound) and left the array's size as a per-Proc
kernel-memory DoS bounded only by graceful OOM. The pagemap is the Linux
page-table-radix shape: a 512-ary tree of 4 KiB nodes, each allocated when
the first slot beneath it is installed, freed again by the take that empties
it, and CHARGED to the address space that caused it when the caller names
one. An untouched reservation costs one small struct however large it is, so
the reservation cap lifted to the burrow window (ARCH 6.5 "Capacity, and the
I-32 default"; B-1a', 2026-09-23) and `page_count` reads as data pages plus
the nodes that index them.

It sits between the Burrow (which embeds one and passes its own lock to every
slot operation) and the buddy ([[sub-kernel-mm-phys]]), and it is deliberately
ignorant of both Procs and permissions: it charges through the address-space
counters ([[sub-kernel-addrspace]]) when handed one and never decides who may
touch a slot. This dossier also owns the chunk's witness suite
(`kernel/test/test_capacity.c`), whose thirteen tests are the range detach's and
the user pool's as much as the pagemap's -- one claim each, listed under Tests.

## Contract

```c
int          pagemap_init(struct pagemap *pm, size_t count);          // 0, or -1: count 0, deeper than 4 levels, slab OOM
bool         pagemap_live(const struct pagemap *pm);                  // count != 0: between init and destroy
struct page *pagemap_get(const struct pagemap *pm, size_t idx);       // the slot's page or NULL; no allocation
size_t       pagemap_resident(const struct pagemap *pm);              // non-NULL slots, O(1)
u32          pagemap_node_count(const struct pagemap *pm);            // node pages allocated (radix only)
int          pagemap_install(struct pagemap *pm, spin_lock_t *lock, size_t idx, struct page *pg,
                             struct AddrSpace *as, bool exempt, struct page **out_winner);
                                                                      // 0 installed / 1 a page was there (*out_winner) / -1 nothing changed
void         pagemap_take(struct pagemap *pm, spin_lock_t *lock, size_t idx, struct AddrSpace *as,
                          struct page **out_pg, struct page **freed, u32 *nfreed);
size_t       pagemap_take_next(struct pagemap *pm, spin_lock_t *lock, size_t from, size_t hi,
                               struct AddrSpace *as, struct page **out_pg, struct page **freed, u32 *nfreed);
                                                                      // the first resident slot in [from, hi), taken; or hi
u64          pagemap_walk_steps(void);                                // node entries pagemap_take_next has visited since boot
bool         pagemap_swap(struct pagemap *pm, spin_lock_t *lock, size_t idx,
                          struct page *expect, struct page *replacement);
int          pagemap_mirror(struct pagemap *dst, const struct pagemap *src, struct page **pool, u32 pool_n,
                            void (*on_page)(struct page *pg, void *ctx), void *ctx);
u32          pagemap_pool_alloc(struct page **pool, u32 n, bool exempt); // nodes for a mirror, from the user pool; < n on OOM or a pool refusal
void         pagemap_pool_free(struct page **pool, u32 n);
void         pagemap_destroy(struct pagemap *pm, void (*put)(struct page *pg, void *ctx), void *ctx);
```

`pagemap_init` chooses the representation from `count` and never changes it:
`count <= PAGEMAP_INLINE_MAX` (32) allocates an INLINE leaf of `count`
pointers from the slab (`KP_ZERO`, uncharged, at most 256 bytes); anything
larger records a `depth` in 1..4 (`depth_for`: the smallest power of 512 that
covers `count`) and allocates nothing -- the root is a node like any other,
absent until the first install. A count beyond 512^4 slots (256 TiB, more
than the whole burrow window) is refused.

`pagemap_install` is INSTALL-ONCE: 0 means `pg` is the slot's page now and
the map owns it; 1 means a page was already there, `*out_winner` names it and
`pg` is still the caller's to free; -1 means out of range, or the path's
nodes could not be had -- buddy OOM, or the charge refused when `as` is
non-NULL -- and NOTHING changed. It takes `lock` for the slot walk and
allocates the missing nodes OUTSIDE it, charging them to `as` (under
`exempt`'s policy) BEFORE allocating, so a cap hit allocates nothing.
`pagemap_take` empties slot `idx` (an absent or out-of-range slot reads as
absent; it never fails) and hands back, in `freed[]` (capacity
`PAGEMAP_MAX_DEPTH`), every node the take emptied, for the caller to
`free_pages(.., 0)` AFTER dropping the lock; those nodes are uncharged from
`as` inside the call. `pagemap_take_next` is the same take applied to the
first resident slot at or above `from` and below `hi` (clamped to `count`),
returning its index or `hi` when the range holds nothing. `pagemap_swap`
replaces the page in a slot iff it still holds `expect`, occupancy
unchanged -- the COW break's commit ([[sub-kernel-addrspace]]).
`pagemap_mirror` fills an init'd, empty `dst` of the SAME count from `src`,
calling `on_page` for every page copied and consuming node pages from a
caller-allocated `pool` (exactly `pagemap_node_count(src)` of them);
`pagemap_destroy` calls `put` for every resident page, frees every node,
frees the inline leaf, and leaves the map not live.

The DATA page is the caller's charge in both directions: the pagemap never
charges or uncharges a slot's page, only the nodes it allocates and frees.

## Mechanism

**The radix.** A node is one `KP_ZERO` page of `PAGEMAP_NODE_ENTRIES` (512)
`void *` entries. Level 0 is the root, level `depth - 1` the leaf, and slot
`idx`'s entry at level `L` is bits `[9 * (depth - 1 - L), +9)` of `idx`
(`level_index`). `pagemap_get` walks it read-only under the caller's lock and
stops at the first absent entry.

**Occupancy is established, never inherited.** Each node's count of non-NULL
entries lives in its own page descriptor's `refcount`, set to 0 by
`node_alloc` at allocation (from `alloc_user_pages(0, KP_ZERO, exempt)` since
the round-1 close: a node is a user page, counted in the physical pool and
returned at `free_pages`, [[sub-kernel-mm-phys]]) -- never read from what the
buddy left, exactly the
`cow_share` rule ([[sub-kernel-mm-phys]] carries the standing warning that
`struct page.refcount` means a different thing to each owner). `path_build`
increments a parent's count when it links a child; the leaf install
increments the leaf's. So a take knows in O(depth) whether it emptied a node,
without scanning 512 entries: `unlink_emptied` walks the recorded path leaf
upward, decrements, stops at the first node still occupied, and unlinks every
node whose count reached zero (the root by NULLing `pm->root`), handing each
back in `freed[]` and settling `pm->nodes` as it goes. An underflow extincts.

**The install is a retry loop, and the buddy is entered with the lock
dropped.** Under `lock`, `path_missing` counts the nodes the path to `idx`
lacks. If the pool the caller has built so far covers them, `path_build`
links the pool nodes into every gap (a linked node is a valid empty node, so
nothing partial is ever undone), the leaf entry is installed or the winner
reported, the lock drops, and any pool nodes a racer made unnecessary are
freed and uncharged. Otherwise the lock drops, the shortfall is CHARGED to
`as` (`addrspace_charge_pages(as, need, exempt)`) and only then allocated
with `node_alloc`; a partial allocation uncharges the part never allocated
and the whole attempt fails -- every pool node freed and uncharged. The pool
is a prefix of at most `PAGEMAP_MAX_DEPTH` entries, consumed from the front.
The inline leaf's install is one locked compare-and-set.

**The take hands nodes back rather than freeing them.** `pagemap_take` and
`pagemap_take_next` unlink under the lock, drop it, uncharge the nodes from
`as` (if non-NULL) and return them in `freed[]`; the caller frees them with
no lock held. `pagemap_destroy` is the one path that frees nodes itself, and
it runs with no lock at all (the Burrow is at `{0,0}`, nothing maps it).

**The walk visits PRESENT nodes, never slots.** `pagemap_take_next` descends
from the root along `idx = from`; at each node it scans forward from `idx`'s
own entry for the first present child, counting every entry it looks at in
`steps`. An exhausted node resumes at the next entry of the nearest ancestor
that has one -- never re-scanning an entry already passed, so nothing below
`from` is ever reached -- and a present entry advances `idx` to the first
slot beneath it (`entry_first_slot`); the leaf's present entry IS the slot
taken, unlinked upward exactly as `pagemap_take` does. A present node always
holds a resident slot beneath it (the take that empties a node unlinks it),
so every step descends toward a page: one call costs O(depth x 512) entry
visits, a range with nothing left O(depth), never O(range). That bound is
what keeps releasing a reservation proportional to what was TOUCHED, and it
matters because a reservation is free and may be the whole window (64 TiB,
2^34 slots): the first WIP of the chunk released per slot, and
`capacity.window_sized_reservation_releases_in_bounded_steps` -- the whole
window reserved, two pages touched, detached -- measured it as the defect it
was before `pagemap_take_next` existed. `g_pagemap_walk_steps` accumulates
`steps` after the unlock (a relaxed atomic add; `pagemap_walk_steps()` reads
it) and is the witness, in the `vma_scan_steps` shape: pinned by a count,
never by the clock. The inline leaf's `take_next` is a linear scan of at most
32 entries and counts nothing.

**The mirror links before it fills.** `mirror_node` takes a pool node,
links it into the parent's slot FIRST, then copies entries: a page entry
runs `on_page` (the clone's `cow_page_get`) and is copied with the leaf's
count bumped; a child entry bumps the parent's count and recurses. A pool
that runs short returns -1 with the link already made (or the parent's count
undone if the link never happened), so the partial tree is reachable whole
and `pagemap_destroy`'s `put` undoes exactly the `on_page` calls that ran.
The pool is exactly `pagemap_node_count(src)` because the source cannot grow
under the mirror: the caller holds the source address space's lock and an
ANON_LAZY Burrow has no other faulter. A short pool is therefore a caller
bug, not a race. `dst` must have the same `count` (hence the same `depth`)
and be empty; an inline source copies its leaf pointer by pointer; a radix
source with no root mirrors to an empty map.

**The lifecycle, by site.** Init: `burrow_create_file` and
`burrow_create_anon_lazy` (`pagemap_init(&v->pm, page_count)`; a failure
frees the struct and returns NULL). Install: the ANON_LAZY miss in
`demand_page_locked` ([[sub-kernel-fault]]) with `p->as` and
`proc_resource_exempt(p)`; the FILE page-in's `file_install_locked` and
`file_install_cluster_locked` with `as = NULL`; `burrow_lazy_populate` (exec's
writable-segment fill) with the mapping address space; and
`burrow_file_install_page_for_test`. Take: `burrow_lazy_populate`'s
all-or-nothing unwind. Take-next: `burrow_release_lazy_range_in`, the
per-mapping release the range detach ([[sub-kernel-vma]]) and
`burrow_decommit_in` both loop over. Swap: `burrow_lazy_swap_slot`, the COW
break's commit. Mirror: `burrow_clone_cow` (Plan 9's `dupseg`), under
`src->lock`, with `clone_take_share` as the hook. Destroy:
`burrow_free_internal`'s FILE arm (`file_put_page`: a plain `free_pages`)
and ANON_LAZY arm (`lazy_put_page`: `cow_page_put`, the buddy only from the
last holder), and `burrow_clone_cow`'s two failure arms.

## Data structures

`struct pagemap` is 32 bytes on AArch64 and embedded in `struct Burrow` as
`pm` (where the 8-byte `filepages` pointer was; no `_Static_assert` pins
either size):

| field | type | meaning |
|---|---|---|
| `count` | `size_t` | slots; 0 == destroyed or never initialized (`pagemap_live`) |
| `depth` | `u32` | 0 = inline leaf; 1..`PAGEMAP_MAX_DEPTH` (4) = radix levels |
| `nodes` | `u32` | node pages currently allocated (radix only) |
| `resident` | `size_t` | non-NULL slots (`pagemap_resident`) |
| `root` | `void *` | inline: `struct page *[count]`; radix: the root node's kva, or NULL |

A node is one 4 KiB page of 512 `void *` entries -- pointers to child nodes
above the leaf level, `struct page *` at it -- reached through
`node_kva(page)` / `node_page(kva)` over the direct map, with its occupancy
in `page->refcount`. The constants: `PAGEMAP_INLINE_MAX` 32,
`PAGEMAP_NODE_SHIFT` 9, `PAGEMAP_NODE_ENTRIES` 512, `PAGEMAP_MAX_DEPTH` 4
(512^4 slots = 256 TiB of pages).

**What a map costs.** Inline: `count * 8` bytes of slab, at init, uncharged
-- bounded per address space by `PROC_VMA_MAX` (65536) x 256 B = 16 MiB,
the same DoS floor the `Vma` slab itself stands on, which keeps mallocng's
many small groups as cheap as they were. Radix: nothing at init; then one
node per level on every path from the root to a resident slot, shared where
paths share prefixes. So a DENSE fill costs at most one leaf per 512 pages
plus the levels above, but a SPARSE one costs up to `depth` nodes per page:
the 4 GiB reservation of `detach.four_gib_reservation_round_trips` (a
2^20-slot, three-level map) touched once every 512 MiB holds 8 pages under
13 nodes -- the root, four level-1 nodes (pairs of touches share one) and
eight leaves. The header states both bounds -- dense, one node per 512 pages plus the
root; sparse, up to `depth` nodes per touched page -- so read the charge as
"data plus the nodes on the touched paths".

## Concurrency

The pagemap has no lock of its own: the Burrow's `v->lock` ([[lock-burrow]])
is passed to every slot operation, and the address-space lock the Burrow's
callers already hold ([[lock-vma]]) is what excludes two faulters of one
address space and any mutator of the mapping. The rule the rest of
`burrow.c` keeps -- **the buddy is never entered under `v->lock`** -- holds
here by construction: `pagemap_install` allocates its nodes with the lock
dropped and frees its leftovers after the unlock; `pagemap_take` /
`take_next` hand emptied nodes back for the caller to free outside;
`pagemap_mirror` runs under `src->lock` on a pool the caller allocated
beforehand and frees the rest afterwards; `pagemap_destroy` runs under no
lock. The only allocation under no lock at all is `pagemap_init`'s inline
leaf. Lock order is therefore unchanged from the pre-pagemap file:
`as->lock -> v->lock -> (nothing)`, with the buddy's own lock taken only
outside both.

The install-once compare under `v->lock` is the audited FILE-arm shape (the
loser frees, the winner installs) and is what makes the install safe against
a sibling faulter that the address-space lock does not exclude -- an
Image-cached FILE Burrow is reachable from several address spaces at once
([[sub-kernel-image]]), which is why a FILE install can genuinely answer 1.
An ANON_LAZY install cannot today (one address space per lazy Burrow; the
fork clones rather than shares), and the arm keeps the check as the
defensive shape.

The retry loop is where the two locks meet: the shortfall is measured under
the lock, allocated outside it, and re-measured under it, because a racer may
have built part of the path in between. The loop terminates because every
iteration either installs or grows the pool by exactly the measured
shortfall, and the shortfall is at most `PAGEMAP_MAX_DEPTH`.

`g_pagemap_walk_steps` is a relaxed atomic, machine-wide, added after the
unlock -- a diagnostic, never a decision.

## Invariants enforced

[[inv-i32]] -- **metadata is charged where the data is.** A node install
charges the address space BEFORE allocating and installs nothing on a
refusal; the take that empties a node uncharges it; `page_count` = data
pages + nodes. This is the invariant's new axis at B-1a': the flat array was
kernel memory proportional to the RESERVATION and charged to nobody, so the
only bound on it was the byte cap the pagemap retired. FILE Burrows pass
`as = NULL` on purpose -- their pages are the Image cache's, shared and
uncharged, bounded by `EXEC_FILE_MAX` (256 MiB of map span) and the cache's
image count -- so their nodes keep the same posture as their pages. The
pool's half of the same invariant is [[sub-kernel-addrspace]]'s.

[[inv-i32]] -- **the conservation law** ([[spec-capacity]]): `pagemap_destroy`
refunds nothing (it has no Proc: `burrow_free_internal` is Proc-agnostic),
which is exactly why every unmapping path releases its slots BEFORE the
Burrow can reach it -- the range detach's phase 3 and the decommit both walk
`pagemap_take_next` over the overlap while the mapping still names the
slots (`NoOrphan`). A slot that lost its mapping while resident would be
charged for the address space's life, and the counter cannot see it
(`ChargeConserved` holds on an orphan). The dying address space's return of
whatever no release path settled is `addrspace_unref`'s.

[[inv-i7]] -- unchanged in substance: a Burrow's pages live until both counts
are zero, and the pagemap is destroyed only from `burrow_free_internal` (or
a never-published clone). The install-once rule keeps a slot's page uniquely
owned by the map (a loser is freed by the caller that made it), and
`pagemap_swap`'s one-for-one replacement keeps occupancy exact across the
COW break.

[[inv-i44]] -- the fork's clone takes exactly one `cow_page_get` per page it
mirrors, in the same hold that writes the slot (`mirror_node`), so the clone
never references a page it has not counted; a short pool's partial mirror is
destroyed through `lazy_put_page`, which puts back exactly the shares taken.
The per-source dedupe that keeps one clone per Burrow is
[[sub-kernel-addrspace]]'s; the share arithmetic is [[spec-cow]]'s.

## Error paths

`pagemap_init` returns -1 for a zero count, a count beyond four levels, or a
slab OOM on the inline leaf; both constructors turn that into NULL having
taken no reference. `pagemap_install` returns -1 having changed nothing: out
of range, a NULL page or lock, the pool's charge refused (`as` non-NULL and
the address space's cap or the user pool hit), or `node_alloc` OOM -- the
callers answer it as a refused fault (`FAULT_UNHANDLED_USER`, the graceful
per-Proc terminate, in both the ANON_LAZY and the FILE arms), as a broken-off
populate (`burrow_lazy_populate` unwinds the run), or as an extinction in the
test helper. `pagemap_take` / `take_next` never fail: out of range reads as
absent, an empty range answers `hi`. `pagemap_swap` answers false for an
absent or changed slot. `pagemap_mirror` answers -1 for a mismatched count,
depth, or a non-empty `dst`, and -1 with `dst` PARTIALLY BUILT for a short
pool -- the caller must destroy `dst`. Two conditions extinct rather than
return: `path_build` running short with a pool the same hold just measured
as sufficient, and a node occupancy underflow in `unlink_emptied` -- each is
the structure declaring its own bookkeeping wrong.

## Performance

`pagemap_get` and `pagemap_swap` are O(depth) pointer chases (at most four).
An install is O(depth) plus at most `depth` page allocations on a cold path,
one lock round-trip when the path exists. A take is O(depth). A `take_next`
is O(depth x 512) per resident slot found and O(depth) for an empty range --
the bound the whole chunk turns on: releasing the 64 TiB reservation with two
pages resident costs a few thousand entry visits where a per-slot release
cost 2^34. `pagemap_resident` and `pagemap_node_count` are O(1), which made
`burrow_lazy_resident_count` O(1) (it used to scan the array under the
lock). A mirror is O(nodes x 512). Memory: above, under Data structures.

## Prosecution

What a reviewer attacks here (the audit-trigger row's addenda; the holotype
round has not run yet):

- **Any pagemap path that allocates or frees under `v->lock`.** The retry
  loop, the leftover free, the take's hand-back and the mirror's pool are
  the four places the discipline is kept; a fifth site that enters the buddy
  under the lock inverts the leaf order.
- **A node whose occupancy is read from an uninitialized descriptor.**
  `node_alloc` establishes 0; a node obtained any other way (a future pool
  source) inherits the buddy's last value and the unlink logic frees a live
  node or never frees a dead one.
- **A charge that reaches the buddy after a refused pool charge.** The order
  is charge, then allocate, then link; a refusal must allocate nothing, and
  a partial allocation must uncharge exactly the part never allocated.
- **Any walk over the pagemap that is O(range).** `pagemap_take_next` is the
  only range walk and it is bounded by present nodes; a caller that loops
  `pagemap_get` or `pagemap_take` per slot over a reservation re-creates the
  2^34 spin the chunk's first WIP had.
- **The leaf-vs-interior confusion.** At level `depth - 1` an entry is a
  `struct page *`; above it, a node kva. `mirror_node` and `destroy_node`
  branch on `level == depth - 1`; an off-by-one dereferences a page's bytes
  as a node.
- **`pagemap_swap` on an absent slot**, or a swap that changes occupancy:
  the break's commit assumes one page for one page.
- **A mirror against a source that can grow.** The pool is sized once; a
  caller that mirrors without holding the source address space's lock turns
  the "caller bug" extinction path into a partial clone at runtime.
- **The DATA charge.** The pagemap never charges a slot's page; a caller
  that assumes it does double-charges (or, worse, never charges) the page.

## Seams

- **The fork clone's nodes were not charged to the child, as first built --
  FIXED in the chunk.** `burrow_clone_cow` allocates its node pool uncharged
  on the promise that the caller charges the clone's footprint
  (`burrow_lazy_footprint`: resident + nodes) after the mapping ref lands;
  `clone_one_vma` ([[sub-kernel-addrspace]]) charged
  `burrow_lazy_resident_count(minted)` and `burrow_lazy_footprint` had no
  caller. The child's takes uncharged the nodes anyway (`pagemap_take_next`
  passes the child's `as`), so a child that decommitted or detached a
  touched clone refunded nodes it never paid for: its `page_count` and the
  user pool drifted BELOW what is held -- the loosening direction, and a
  fork-and-detach loop could walk the machine-wide bound away. Found by the
  chunk's self-audit and, independently, by this documentation pass; one
  call closed it, and `capacity.fork_clone_charges_pages_and_nodes` counts
  the nodes across a fork (its RED, `noclonefootprint`, is the bug itself).

## Caveats

`pagemap_get` takes no lock and asserts nothing about one: it reads a
structure that `install` / `take` mutate under `v->lock`, so a caller must
hold that lock (or have the map quiesced, the clone's case) or accept a torn
read. The `const` on its parameter is about the map's contents, not its
synchronization.

`pagemap_install`'s `exempt` is consulted only for the NODE charge; it does
not make the install skip the charge, it makes the charge unrefusable. FILE
callers that want no charge at all pass `as = NULL`, not `exempt = true`.

The header's cost summary overstates the bound for sparse touches (Data
structures, above).

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)

## Tests

`kernel/test/test_capacity.c` (B-1a'; thirteen tests, one claim each, registered
in `kernel/test/test.c`; the suite ran 1650/1650 at `-smp 4` and `-smp 1` on
the chunk's WIP tip, per its commit message):

- `detach.range_trims_left_right_middle` -- a range cutting a lazy mapping's
  head, tail or middle leaves survivors whose byte identity, pages and PTEs
  are untouched, releases exactly the cut pages (`page_count`, the resident
  count, the PTEs), and a range that maps nothing answers 0.
- `detach.range_across_burrows_and_holes` -- one range trims a mapping's
  tail, removes a whole one and trims another's head across two holes; the
  whole one's slots are released BEFORE its mapping goes (a held Burrow ref
  sees it alive with nothing resident) -- `NoOrphan`.
- `detach.range_refusals_change_nothing` -- a CODE alias anywhere (-1 /
  EACCES), a cut shared-in mapping (-1 / EACCES) and a middle cut at
  `PROC_VMA_MAX` (-1 / ENOMEM) each leave pages, PTEs, counts and geometry as
  they were; a whole shared-in mapping goes and refunds `shared_map_pages`;
  a head trim at the cap is served (no slot needed); malformed / below-window
  munmaps answer EINVAL / ENOSYS.
- `detach.eager_pages_go_with_the_last_piece` -- an eager region cut into
  pieces stays charged until its LAST piece goes.
- `detach.lazy_over_256mib_detaches` -- a 512 MiB lazy attach and a 512 MiB
  reserve both detach (the old detach refused any length over
  `BURROW_ATTACH_MAX`).
- `detach.four_gib_reservation_round_trips` -- the browser-status exit:
  4 GiB reserved, touched every 512 MiB (8 pages + 13 nodes charged), a GiB
  protected to R, a 2 GiB range detached across the pieces (4 pages + 6
  nodes back), then the rest: 0 mappings, 0 charged, the physical free count
  back.
- `capacity.window_sized_reservation_releases_in_bounded_steps` -- the whole
  window reserved, two pages touched, detached: `pagemap_walk_steps` moves
  by a few thousand, not 2^34.
- `capacity.replace_window_releases_orphans` -- a MAP_FIXED window inside a
  touched lazy mapping releases the window's slots before the swap (the
  B-1a audit's F5).
- `capacity.pagemap_nodes_charged_and_reclaimed` -- a 1 GiB map's nodes are
  charged as touched (root + leaf, then a second leaf) and reclaimed as
  decommitted, root included.
- `capacity.default_is_ram_minus_reserve` -- reserve = max(65536, RAM/8)
  clamped to RAM/2; pool = RAM - reserve; the default budget and the hard
  maximum are the pool; a fresh Proc and its address space carry it.
- `capacity.pool_refuses_users_keeps_tcb` -- with the pool parked at K
  pages, a user Proc far below its own budget is refused at exactly K --
  pages, nodes and tables all counted; the TCB is not; every page returns.
- `capacity.death_returns_charges_to_pool` -- a Proc that dies holding
  pages, nodes, tables and an eager region returns all of it to the pool --
  through the drain's frees, not a death return (the round-1 close).
- `capacity.page_tables_charged_and_reclaimed`,
  `capacity.memory_bomb_leaves_the_reserve`,
  `capacity.fork_costs_the_pool_only_its_nodes` -- the round-1 close's three
  ([[sub-kernel-mmu]], [[sub-kernel-mm-phys]]); sixteen in all.
- `capacity.fork_clone_charges_pages_and_nodes` -- a parent with two touched
  GiB maps (3 pages + 5 nodes) is cloned; the child is charged 8 (pages AND
  the mirrored nodes), a detach of one map in the child refunds exactly that
  map's footprint, the parent is untouched and the pool is exact at every
  step; the child's death returns the rest.

Two of those tests found defects in the chunk's own first build, both fixed
before this dossier was written: the death test found that `addrspace_unref`
let a dying address space's leftover `page_count` die with it -- harmless
while the counter was the only bound, a permanent shrinking of the
machine-wide pool once one existed; and the window-sized test found the
release walking every slot of the range (2^34 for the window) before
`pagemap_take_next` existed. Neither is an audit finding.

Older tests re-expressed by the chunk: `test_exec.c`'s charge assertions
read data + the nodes the two maps hold (root + leaf for the 1024-slot
segment, one node for the 256-slot stack) rather than a restated constant;
`test_demand_page.c`, `test_addrspace.c`, `test_cow.c` and `test_protect.c`
park counts at `page_budget` / seed with `proc_default_page_budget()` where
they named `PROC_PAGE_MAX`. The range-detach re-expressions in
`test_sys_burrow.c` and `test_protect.c` are listed on [[sub-kernel-vma]].

The EL0 witness is `/capacity-probe` ([[sub-kernel-protect-witness]]), and
the model is [[spec-capacity]] (`specs/check-capacity.sh`). No sabotage
(RED) runs have been recorded for this suite yet.
