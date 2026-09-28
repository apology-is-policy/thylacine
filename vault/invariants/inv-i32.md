---
id: inv-i32
type: inv
title: "I-32 — the resource floor (per-Proc, and per-address-space since L-2)"
number: I-32
guards: [sub-kernel-proc, sub-kernel-hwcap, sub-kernel-content, sub-kernel-addrspace, sub-kernel-pagemap, sub-kernel-mm-phys, sub-kernel-mmu]
validated-by: [gate-smp, spec-capacity]
strength: prose
created: 2026-08-01
updated: 2026-09-23
---
## Statement

A non-TCB Proc's resource use is bounded on every axis a hostile or buggy
program can drive, so a fork bomb / thread bomb / memory bomb hits a clean
limit instead of stressing the allocator toward the box-killing
cliff. On a bound, creation fails cleanly (`-ENOMEM` / `-EAGAIN` / a refused
fault) — it never box-extincts.

The invariant is still named "per-Proc" and that is now only half right: the
memory axes bound an **address space**, which one or several Procs may share.
See the enforcement table.

This is a **resource** axis, deliberately orthogonal to authority: it is not
a privilege gate and it composes with, rather than substitutes for, the
capability model.

## Enforcement

Five axes — but they are no longer counted in one place, and since the
address-space extraction three of them are **not per-Proc at all**:

| Axis | Counter | Lives on | Cap | Charged under |
|---|---|---|---|---|
| anon pages | `page_count` (+ `page_peak`) | `AddrSpace` | `AddrSpace.page_budget` — the *enforced* cap, seeded from the creating Proc's `page_budget` *authorization*; ≤ `proc_page_budget_hard_max()` = the user pool (B-1a'); the pool itself is PHYSICAL since the round-1 close -- charged where the page is allocated, returned where it is freed ([[sub-kernel-mm-phys]]) -- and the hardware page tables and the mapped file pages are inside `page_count` (`pgtable_pages` / `file_pages` tell them apart) | [[lock-vma]] — exact *against a sibling on the same address space* |
| live VMAs | `vma_count` | `AddrSpace` | `PROC_VMA_MAX` | [[lock-vma]] — same |
| shared-in pages | `shared_map_pages` | `AddrSpace` | `PROC_SHARED_MAP_MAX_PAGES` | [[lock-vma]] — same |
| direct children | `child_count` | `Proc` | `PROC_CHILD_MAX` | [[lock-proc-table]] — bounded overshoot |
| live threads | `thread_count` (+ the poll-thread count) | `Proc` | `PROC_THREAD_MAX` | [[lock-proc-table]] — bounded overshoot |

**The first three moved with the mapping list, and that changed what they
bound.** Two Procs sharing an address space share its pages, so one charge is
the honest count and the per-Proc cap becomes a **per-address-space** cap. The
fork bomb stays bounded by a different argument than before: N children means N
address spaces, each capped separately. Keeping the counters per-Proc would have
given two sharers divergent counts for one region set, and left the uncharge —
which runs off the mapping list — with no way to know whose counter to
decrement.

**The cap had to move with the counter, and the argument for that is the best
line in this invariant.** Counting on the address space while capping on the
Proc was considered and rejected: two siblings sharing one counter would then
return *different verdicts* about it, making the effective bound depend on which
sibling happened to fault first. A resource bound whose value depends on
scheduling is not a bound. So the enforced ceiling lives beside the counter, and
the Proc keeps only the *authorization* it seeds a new address space from — the
same split as everywhere else in this system, where the right to confer and the
thing conferred are different objects. The high-water mark moved for a plainer
reason: separated from the counter it mirrors, it had already come close to
being silently reverted by a merge.

**"Exact" is narrower than it reads, and the note used to overstate it.** The
arithmetic is a compare-and-swap loop and assumes *no* lock, because the
uncharge sits where the pages actually free — a ring's pages are released from a
handle close, which holds no address-space lock, and a sibling's attach can
interleave. What the lock buys is the **cap decision**: holding it across
check-then-charge makes the bound exact against another charge on the same
address space. Two charges from *outside* it can both pass and both land,
overshooting by at most the smaller. The compare-and-swap guarantees no update
is ever *lost*, which is the property an accounting bound genuinely cannot do
without.

So the honest reading is the one the code states: **a floor, not an accountant**
— on all five axes, differing only in the size of the tolerated overshoot. The
two creation gates read under the table lock and increment at a later hold,
carrying a window of at most one per concurrently-spawning CPU.

`bounce_bytes` (the transient byte-I/O staging heap) is an I-32-shaped sixth
axis with a softer failure mode — over budget *degrades* to the stack tier,
producing a short op rather than a failed one.

**DMA buffers are on none of these axes.** A driver's DMA pages come from the
same allocator as anon pages but are charged to no counter, so `page_count` is
not the true page footprint of any Proc holding hardware. The bound is elsewhere
and differently shaped: the allowance's **per-buffer** ceiling
([[sub-kernel-hwcap]]), which caps one buffer rather than their sum. That shape
is structural rather than accidental — [[inv-i34]]'s conferred set carries a
single maximum size, so there is nowhere in its data model for a sum to live,
and a cumulative budget would have to extend the model rather than add a check.
It is a recorded future item. The gap is narrow today because the
capability to create a DMA buffer is itself tightly held — so the bound is on
*who may ask*, not on how much they may accumulate, which is a different kind of
floor from the rest of this invariant and worth stating rather than assuming.

**The exemption is the load-bearing part.** `proc_resource_exempt` is
`principal_id == PRINCIPAL_SYSTEM` — the TCB, so the floor cannot pinch the
FS server, the orphan-adopter, or the kthread root. It is unforgeable
because `CAP_SET_IDENTITY` refuses to stamp `PRINCIPAL_SYSTEM` (and
`proc_apply_identity` extincts on the attempt), and `principal_id` is
immutable on a running Proc — so a plain read is sound. A NULL Proc reads
non-exempt (fail-closed).

The **graceful-OOM backstop** is what bounds the recursive case: every user
creation path (`proc_alloc`, `thread_create`, `territory_clone`,
`burrow_create_anon`, the demand-page install) returns an error or
per-Proc-terminates, never a box extinction. So a bomb that evades a
per-Proc cap by spreading across Procs still terminates at the physical
cliff instead of taking the machine.

**Subsystem-local bounds are the same invariant at a smaller scale, and they are
not on this table.** The per-Proc environment ([[sub-kernel-content]]) caps its
variable count and each value's length, which bounds a hostile program's kernel
allocation there to a quarter-megabyte per Proc. These are not counted on
`struct Proc` and never reach the axes above — they are enforced where the
allocation happens, by the structure that owns it. That is the general shape:
this invariant's table lists the axes with a *shared* allocator behind them, and
each subsystem that allocates on a Proc's behalf carries its own ceiling. A
reader auditing "is this Proc bounded" must therefore consult both, and the table
alone will read as more complete than it is.

`page_budget` (CL-5) makes the page axis scoped rather than global:
inherited across `rfork` (load-bearing — `make` and `clang` are Pouch ports
that know nothing of budgets, so only inheritance carries a raise from the
build root down to `cc1`), freely *lowered* by any spawner (monotonic
reduction, the I-2 shape), raised only with
`PROC_FLAG_MAY_RAISE_PAGE_BUDGET`, and never above the hard cap by any
authority. Read it as the **authorization** half of the split above: what a Proc
may seed into an address space, not what any address space is currently held to.

**The user pool is the second, machine-wide bound above every cap, and the
default cap IS the pool** (B-1a', 2026-09-23; ARCH 6.5 "Capacity, and the
I-32 default"). The bar the operator set is production-comparable memory
management: a program is never refused memory while free memory exists, and
memory it relinquishes returns so its footprint shrinks. So the DEFAULT budget
of an unconfined address space is RAM minus a TCB reserve of max(256 MiB,
RAM/8) clamped to RAM/2, sized once at boot (`capacity_init`;
[[sub-kernel-addrspace]]); `proc_default_page_budget()` =
`proc_page_budget_hard_max()` = that pool, and the constants `PROC_PAGE_MAX`
(256 MiB) and `PROC_PAGE_HARD_MAX` (4 GiB) no longer exist -- the cap is the
CONFINEMENT mechanism a parent narrows a child with (containers, browser
content Procs), no longer the everyday bound. What keeps the floor is the pool
itself, and since the round-1 close it is PHYSICAL ([[sub-kernel-mm-phys]]
"The user pool"): every user page -- data, pagemap node, page table, ring --
is charged to it where it is ALLOCATED (`alloc_user_pages`; a non-exempt
allocation refused before the buddy is entered when it would take the pool
past its size; a `PRINCIPAL_SYSTEM` allocation counted but never refused, the
reserve being theirs) and returned where it is FREED (`free_pages` of a
`PG_USER` page, whoever frees it), so a COW-shared page is one charge and a
fork costs the pool only its node mirror while each address space's
`page_count` keeps the holder reading under its cap (the Linux memcg shape;
the counter-shaped first build refused a fork with half the pool free, the
round-1 audit's F5). N Procs each within the default therefore
cannot together reach the reserve, user Procs are refused cleanly at
exhaustion while the TCB keeps allocating, and the fault-time policy --
terminate the faulting non-TCB Proc -- no longer risks the TCB; no OOM victim
selection is built. Four consequences on the page axis: **metadata is charged
where the data is** -- the pagemap's node pages ([[sub-kernel-pagemap]]) are
charged to the address space that touched them, so `page_count` reads as data
plus the nodes that index it and a reservation's byte size is no longer a
resource (`BURROW_RESERVE_MAX` is the burrow window); **the hardware page
tables are charged and reclaimed** (the round-1 audit's F1;
[[sub-kernel-mmu]]): every L1 / L2 / L3 a touch grows is charged to the space
(`addrspace_charge_table`) and taken from the pool before it is linked, and a
clear that empties a table frees it up to the L0 entry, so a decommitted
region's whole footprint returns -- uncharged, death-reclaimed tables were the
hole that let a Proc touching one page per 2 MiB and decommitting empty the
buddy at a charged count of three; and **a dying address space returns
nothing by itself**, because the drain's frees return page by page (the death
return of the first build would now double-return); and, from the round-2
audit's F8, **the pool reclaims before it refuses, and a mapped file page is
charged to its holder** -- the Image cache's pages are pool memory, charged
to each address space that maps them per leaf (`addrspace_charge_file`;
`file:`), and a non-exempt charge that would be refused first strips idle
images (cached, mapped by no one) of their pages, least recently used first,
then asks again (`capacity_set_reclaim`, `image_cache_reclaim`;
[[sub-kernel-image]]), so a dead Proc's cached text cannot hold the pool and
a confined Proc's text counts against its cap. From round 3 (F12 / F13 /
F15): the copy-on-write copy keeps its share of the original until the leaf
that could still translate to it is replaced; a fault that finds a leaf
already admitting its access is answered by that leaf, never terminated for
a mismatch -- fail clean covers the benign race too; and a refused
allocation inside exec reports `-T_E_NOMEM`, not a malformed-binary code.
From round 4 (F17 / F18 / F19): an EL0 abort no page install can resolve --
an alignment fault or a synchronous external abort on a mapped page -- is
refused on its class before any lookup and the Proc dies with `snare:bus`,
where it used to be answered as handled and re-fault forever (a livelock is
not fail clean); the pool's `-T_E_NOMEM` reaches the exec frame's populate
and the VMA cap as it reaches the segments; and a refused charge asks the
reclaim for its whole shortfall at once. The
accounting law that keeps the counter refundable -- every path that unmaps a
slot releases it FIRST, since `burrow_free_internal` refunds nothing -- is
[[spec-capacity]]'s `NoOrphan`, kept by the range detach's release-before-
reshape order ([[sub-kernel-vma]]). The fork clone's node pages ARE charged
to the child (`burrow_lazy_footprint`; fixed in the chunk, the round-1 audit's
F2).

## Validation

Prose + the focused audits; [[gate-smp]] for the counter races; since B-1a'
the page axis's conservation law is [[spec-capacity]] (`ChargeConserved`,
`NoOrphan`) and the pool, the metadata charge, the table charge and the
physical return are the sixteen `test_capacity` tests'
([[sub-kernel-pagemap]]; `capacity.memory_bomb_leaves_the_reserve` is the
round-1 attack refused), the holder charge and the reclaim the two round-2
`demand_page` tests' (`file_pages_charge_the_holder`,
`idle_image_reclaimed_under_pressure`). **blind-to:** there is
a machine-wide aggregate now (the pool) but still no per-user one, so a
cgroup-equivalent remains a recorded seam; the spec sees one address space and
no metadata; the two creation gates' overshoot is real and deliberate;
`page_peak` is telemetry only; no policy reads it.
