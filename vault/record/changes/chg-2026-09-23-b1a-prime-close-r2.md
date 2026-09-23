---
id: chg-2026-09-23-b1a-prime-close-r2
type: chg
title: "B-1a' (capacity), the round-2 close: the pool reclaims idle images before it refuses, a mapped file page is charged to its holder, the copy-on-write break replaces its leaf in place, a fork keeps the parent's tables -- round 2's four findings closed, round 3 on the fixes"
date: 2026-09-23
arc: arc-boosty
commits: ["387ffcd8"]
touched:
  - sub-kernel-mm-phys
  - sub-kernel-mmu
  - sub-kernel-addrspace
  - sub-kernel-fault
  - sub-kernel-pagemap
  - sub-kernel-burrow
  - sub-kernel-image
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
supersedes: chg-2026-09-23-b1a-prime-close
depth: rich
created: 2026-09-23
---
## What the superseded note said, and what changed

[[chg-2026-09-23-b1a-prime-close]] records the chunk through the round-1
close -- the pool physical, the tables charged and reclaimed, F3 / F4 / F6 /
F7 -- and round 2's four findings "in flight, their record appended at the
landing". This note is that record: the fixes are built, tested, and under a
round 3; everything the superseded note says about the round-1 close stands.

## The round-2 close

[[adt-b1a-prime-r2]] (Fable 5.1, start == end) on the round-1 close's code
returned 0 P0 / 0 P1 / 2 P2 / 2 P3. **F8** ([[fnd-b1a-prime-r2-f8]]): the
Image cache was a pool hoard nobody paid for -- a FILE page-in pool-charged
but counted against no address space, surviving its toucher's death as an
idle cache entry that nothing reclaimed until 120 more distinct images
evicted it, so a dead Proc could hold the whole physical pool while
free-able memory existed; and a confined Proc never saw its own text on its
cap. Two heritage halves, auto-accepted under the operator-away grant. The
POOL RECLAIMS BEFORE IT REFUSES ([[sub-kernel-mm-phys]] "The pool reclaims
before it refuses"; [[sub-kernel-image]] "The cache is the pool's reserve
under pressure"): `capacity_set_reclaim(fn)` registers the Image cache's
`image_cache_reclaim` with the allocator; `alloc_user_pages` asks it for the
pages it needs each time `pool_charge` refuses a non-exempt charge and asks
the pool again; under `g_image_lock` the least recently used idle entries --
one handle, no mapping, resident pages -- are stripped of their pages
(`burrow_image_strip`, [[sub-kernel-burrow]]) and left cached and empty for
the next mapper to page in again (Plan 9's `imagereclaim`; Linux's page cache
giving way to anonymous demand); the eviction proof's {1, 0} idleness under
that lock is what makes freeing a page nothing else names safe; the lock
order is [`as->lock` ->] `g_image_lock` -> `v->lock` -> the buddy and no
allocator caller holds any of them. A MAPPED FILE PAGE IS CHARGED TO ITS
HOLDER ([[sub-kernel-fault]] "A file page is the cache's; its mapping is the
holder's"; [[sub-kernel-addrspace]]): every FILE leaf install is charged to
the mapping space per leaf (`addrspace_charge_file`; `page_count` + a
`file_pages` telemetry; refunded when the install finds the leaf already
there) and refunded per leaf a range clear removes -- `vma_uninstall_range_in`
([[sub-kernel-vma]] "The round-2 close: the range clear per mapping"), now
the one clear the detach's phase 2, the protect, the unmap and the decommit
use; `/proc/<pid>/status` prints `file:` beside `tables:`
([[sub-kernel-devproc]]) and the capacity probe's census is `pages:` minus
both ([[sub-kernel-protect-witness]]). **F9** ([[fnd-b1a-prime-r2-f9]]): the
copy-on-write break's unconditional uninstall -- load-bearing under the old
allocator -- freed the leaf's tables under the round-1 close's reclaim and
the re-install re-allocated them against a pool it held no lock on: a
pool-edge termination race for a page the Proc already held, and a table
churn per break; the clone's phase 1 reclaimed every table under every COW
range. Fixed by `mmu_replace_user_pte_attr` ([[sub-kernel-mmu]] "The COW
break replaces its leaf in place"): a break-before-make on the leaf alone,
occupancy untouched, 1 when identical, the install when nothing is mapped;
the write arm sets `cow_replace` and the uninstall is gone; and by
`mmu_uninstall_user_range_keep_tables` for the clone's phase 1: the parent's
emptied tables stay linked at occupancy 0, charged, for the re-faults to find,
as Linux keeps them across fork. `mmu_install_user_pte_attr` returns 1 on the
idempotent re-install; `mmu_uninstall_user_range` returns the leaves it
cleared. **F10**: a pool refusal at `SYS_BURROW_ATTACH` and `SYS_LOOM_SETUP`
returns `-T_E_NOMEM`, not `-1`. **F11**: `devproc.c`'s comments say the
holder count; the manual text and a `/ctl/procs` column stay with the prowl
telemetry sub-chunk.

Two tests joined `test_demand_page`: `demand_page.file_pages_charge_the_holder`
(two leaves charge two, a re-fault charges nothing, the in-kernel range
detach refunds both while the Burrow keeps both pages resident) and
`demand_page.idle_image_reclaimed_under_pressure` (an idle image with two
resident pages, the pool parked full, one user allocation served by one
reclaim that freed exactly those two, the entry still the one the next lookup
returns). Figures re-derived for keep-tables in `capacity.fork_clone_charges_
pages_and_nodes` and `capacity.fork_costs_the_pool_only_its_nodes` (the
parent's tables stay); `cow.break_sole_holder_takes_in_place` pins the pool
AND `pgtable_pages` unchanged across the break; the protect's range uninstall
answers 1 for its one leaf; the idempotent install answers 1; the three
pool-parking capacity tests evict idle images first. The RED harness's
`notablereclaim` anchor follows the new line.

## Round 3

Round 3 (Fable 5.1) on the round-2 fixes -- the reclaim's lock order and
reentrancy, the {1, 0} idleness proof under the strip on every teardown path,
the holder charge / refund pairing on every install and clear, the leaf
replace's break-before-make, the durable linked-at-occupancy-0 table state
keep-tables introduced, the `-T_E_NOMEM` sweep, the probe's census -- is
recorded at the landing.

## Verification (re-taken on the final tree)

The kernel suite 1656/1656 at `-smp 4` and at `-smp 1` with joey clean (CL-5
OK, `capacity-probe: ALL OK`, net-8a PASS) on the round-2 fixes. The nine REDs
and the SMP gate run on the final tree.
