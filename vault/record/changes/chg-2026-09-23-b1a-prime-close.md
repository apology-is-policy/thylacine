---
id: chg-2026-09-23-b1a-prime-close
type: chg
title: "B-1a' (capacity), the audit close: the pool made PHYSICAL (charged at allocation, returned at free), the hardware page tables charged and reclaimed, F3/F4/F6/F7 -- round 1's seven findings closed, round 2's four in flight"
date: 2026-09-23
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-kernel-mm-phys
  - sub-kernel-mmu
  - sub-kernel-addrspace
  - sub-kernel-fault
  - sub-kernel-pagemap
  - sub-kernel-burrow
  - sub-kernel-vma
  - sub-kernel-devproc
  - sub-kernel-loom
  - sub-kernel-exec
  - sub-netd-server
  - sub-kernel-protect-witness
  - moc-kernel-memory
  - inv-i32
  - spec-capacity
established: []
closed: []
opened: []
supersedes: chg-2026-09-23-b1a-prime-capacity
depth: rich
created: 2026-09-23
---
## What the superseded note said, and what changed

[[chg-2026-09-23-b1a-prime-capacity]] records the chunk as built through WIP 4:
the user pool as a COUNTER in `kernel/addrspace.c` above `page_count`, charged
before the space's own cap, with a dying address space returning its leftover
count -- and "the holotype audit has NOT run". Both are superseded here: the
holotype round 1 ran on that tree, and its close moved the pool.

## The round-1 close (d04189fb on the WIP branch)

[[adt-b1a-prime-r1]] (Fable 5.1, start == end) returned 0 P0 / 2 P1 / 0 P2 /
5 P3 on the WIP-3 tree. F2 [P1], the fork clone charging the child resident
pages while it mirrors nodes too, the chunk's own self-audit had already fixed
(`burrow_lazy_footprint`, WIP 4; [[fnd-b1a-prime-r1-f2]]). F1 [P1] is a
SYSTEM finding ([[fnd-b1a-prime-r1-f1]]): hardware page tables under a user
L0 were allocated uncharged and never reclaimed before death, so with the
reservation cap at the window one page per 2 MiB touched and decommitted left
one unreclaimable table page per iteration -- the buddy empty at a charged
count of three. The tables predate the chunk (attribution); the chunk removed
the last incidental cost of the attack and its scripture claim rested on them
(ownership). F5 [P3] named a design tension honestly: a holder-counted pool
refused a fork of a Proc holding more than half the room while free memory
existed, against the ratified bar.

One move resolved F5 and gave F1 its return path: **the pool became
PHYSICAL** ([[sub-kernel-mm-phys]] "The user pool") -- `alloc_user_pages`
charges it at allocation and tags the head page `PG_USER`; `free_pages`, the
one place a page can leave, returns the charge whoever frees it; the
per-address-space count keeps holder semantics under the cap (the Linux
memcg shape); WIP 3's death return in `addrspace_unref` is gone, because it
would double-return. Every user-page allocation site converted; the creators
gained `exempt` (`burrow_create_anon` / `_code`, `burrow_clone_cow`,
`loom_create`, `pagemap_pool_alloc`), so the TCB's own Burrows are counted
but never refused. **F1 as built** ([[sub-kernel-mmu]] "User page tables are
charged and reclaimed"): each table charged (`addrspace_charge_table`:
`page_count` + the new `pgtable_pages`) and pool-allocated before it is
linked; occupancy in `page->refcount`, established at 0; a clear that empties
a table unlinks it (invalid, `dsb ishst`, `tlbi vaae1is` on one VA, `dsb
ish`), re-reads the 512 entries (extinction on a live one), frees and
uncharges it, then its parent; a failed install unwinds what it linked;
`mmu_install_user_pte*` take `(as, exempt)`, the uninstalls `as`. **F3**:
`burrow_release_lazy_range_in` ends on the take's answer and `vma_alloc`
refuses a mapping past its Burrow's end. **F4**: `Burrow.charge_as_id` keyed
on a new `AddrSpace.id`. **F6**: netd's retirement oracle asks RW. **F7**: the
default-budget test asserts falsifiable facts and pins the 2 GiB guest.
`/proc/<pid>/status` gained `tables:` ([[sub-kernel-devproc]]); the capacity
probe's census is `pages:` minus it. Three tests joined `test_capacity`
(sixteen): `capacity.page_tables_charged_and_reclaimed`,
`capacity.memory_bomb_leaves_the_reserve` (the round-1 attack refused within
one touch's cost of the room, tables counted, everything returned by the
decommit, the second round refused at the same point),
`capacity.fork_costs_the_pool_only_its_nodes`; seven figures in
`test_demand_page` / `test_cow` / `test_protect` became the data view
(`page_count - pgtable_pages`). The REDs are nine (`nopool` moved to
`mm/phys.c`; `nodeathrefund` retired with its mechanism; `nofreereturn`,
`notablecharge`, `notablereclaim` new).

## Round 2, and what it found in the close

Round 2 (Fable 5.1, start == end) on the close's code returned 0 P0 / 0 P1 /
2 P2 / 2 P3: F8, the Image cache as a pool hoard -- a user's file pages are
pool-charged but counted against no address space, survive its death in the
cache's idle entries, and nothing reclaims them, so a dead Proc can leave the
pool full while free-able memory exists; F9, the copy-on-write break's
unconditional uninstall now frees the leaf's tables and the re-install
re-allocates them from a pool it does not hold `as->lock` against, so at the
pool edge a Proc can be terminated for writing a page it already holds; F10, a
pool refusal at attach / Loom setup surfacing as `-1`; F11, `pages:` now
carrying tables with two comments saying otherwise. The fixes -- a reclaim
step that strips idle images of their pages when a non-exempt allocation would
be refused (Plan 9's `imagereclaim`, Linux's page-cache reclaim), file pages
charged to the mapping space per PTE like COW pages, a leaf-level replace for
the break, the parent's tables kept across a fork -- are the next WIP; their
record is appended at the landing.

## Verification (as recorded by the close, re-taken on the final tree)

The kernel suite 1654/1654 at `-smp 4` with joey clean (CL-5 OK,
`capacity-probe: ALL OK`, net-8a PASS) on the close's tree; 1654/1654 at
`-smp 1` one edit earlier. Six of the nine REDs re-run on d04189fb:
nodetachrefund reddens 14 tests, nonodecharge 84, nopool exactly the four pool
tests, nofreereturn 88, notablecharge 85, notablereclaim 89. The full nine and
the SMP gate run on the final tree.
