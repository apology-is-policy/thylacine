---
id: fnd-b1a-prime-r2-f9
type: fnd
title: "The copy-on-write break tears its tables down and rebuilds them in one fault: the unconditional uninstall frees the leaf's tables to the pool and the re-install re-allocates them against a pool it holds no lock on -- a pool-edge kill for a page the Proc already holds"
round: adt-b1a-prime-r2
severity: P2
status: fixed
surface: [sub-kernel-mmu, sub-kernel-fault, sub-kernel-addrspace]
threatens: [inv-i32, inv-i44]
fixed-by: chg-2026-09-23-b1a-prime-close-r2
regression: "cow.break_sole_holder_takes_in_place (the pool and pgtable_pages unchanged across the break); capacity.fork_clone_charges_pages_and_nodes, capacity.fork_costs_the_pool_only_its_nodes (the parent's tables stay across the fork)"
created: 2026-09-23
---
## Prosecution

`arch/arm64/fault.c`'s write arm called `mmu_uninstall_user_pte(p->as,
page_va)` unconditionally before the break, because the install refuses a
mismatching valid leaf and both break outcomes mismatch. With the round-1
close's table reclaim, that clear emptied the leaf's L3 and freed it (and any
emptied ancestor) to the pool; the sole-holder or copy install then
re-allocated the path through `user_table_alloc` -> `pool_charge`, which
respects no lock the faulter holds. At the pool's edge a peer's allocation
takes the released pages between the two and the Proc is terminated for
writing a page it already holds -- SMP-only, non-deterministic. Cost besides:
every sparse break paid a table free (TLBI + the 512-entry verify scan) and an
allocation, and `addrspace_clone`'s phase 1 reclaimed every table under every
COW range, so a forked parent rebuilt all of them one fault at a time (Linux
modifies the PTE in place and keeps the parent's tables across fork).

## Fix

`mmu_replace_user_pte_attr(as, exempt, va, pa, prot, mair)`
([[sub-kernel-mmu]] "The COW break replaces its leaf in place"): a
non-growing walk; anything short of a valid leaf is the plain install; an
identical leaf is 1; otherwise a break-before-make on the leaf alone --
invalid, TLBI, the new entry, TLBI -- with the table's occupancy untouched and
nothing freed or allocated. The write arm sets `cow_replace` and step 5 uses
it; the unconditional uninstall is gone ([[sub-kernel-fault]]).
`mmu_uninstall_user_range_keep_tables` (a static `uninstall_range(as, lo, hi,
reclaim)` behind both range forms, which now return the leaves they cleared)
is the clone's phase-1 clear: the parent's emptied tables stay linked at
occupancy 0, still charged, for the re-faults to find, reclaimed by the next
range clear over them or at death ([[sub-kernel-addrspace]]).
`mmu_install_user_pte_attr` returns 1 on the idempotent re-install so a caller
that charged for the leaf can refund it; callers check `rc < 0`.
