---
id: fnd-b1a-prime-r1-f1
type: fnd
title: "The reserve is not a reserve: user page tables are allocated uncharged and never reclaimed before death, so one touch per 2 MiB, decommitted, empties the buddy at a charged count of three"
round: adt-b1a-prime-r1
severity: P1
status: fixed
surface: [sub-kernel-mmu, sub-kernel-addrspace, sub-kernel-mm-phys]
threatens: [inv-i32]
fixed-by: chg-2026-09-23-b1a-prime-capacity
regression: "capacity.page_tables_charged_and_reclaimed, capacity.memory_bomb_leaves_the_reserve; REDs notablecharge / notablereclaim"
created: 2026-09-23
---
## Prosecution

`mmu_install_user_pte`'s walk-and-grow allocated every L1 / L2 / L3 under a user
L0 with a bare `alloc_pages`, charged to nothing, and the only table free was
`proc_pgtable_destroy` at address-space death (`mmu_uninstall_user_range`
cleared leaves only). With `BURROW_RESERVE_MAX` lifted to the window, an
unprivileged Proc reserves 64 TiB, touches one page per 2 MiB (one page and at
most three pagemap nodes charged; one L3 table allocated, an L2 per GiB) and
decommits it: the page and the nodes come back, the table stays. One uncharged,
unreclaimable page per iteration, `capacity_pool_charged()` ~3 throughout, the
buddy empty after RAM / 4 KiB iterations, and the next `PRINCIPAL_SYSTEM`
allocation fails. Without any decommit the tables allocate 1:1 with charged
pages, so the buddy empties at charged ~RAM/2, below the pool. Attribution:
the uncharged, death-reclaimed tables predate the chunk; ownership: the chunk
removed the last incidental cost of the attack and its scripture claim rested on
them.

## Fix

Tables are charged to the address space (`addrspace_charge_table`: `page_count`
plus a `pgtable_pages` telemetry field) and taken from the user pool
(`alloc_user_pages`) BEFORE they are linked; occupancy lives in the table page's
own `refcount`, established at 0; a clear that empties a table unlinks it
(break-before-make on the table descriptor: invalid, `dsb ishst`, `tlbi vaae1is`
on one VA under it, `dsb ish`), re-reads its 512 entries and extincts on a live
one, frees it (the pool takes the page back at `free_pages`) and uncharges it,
then asks the same of its parent up to the L0 entry; a failed install unwinds
the tables it linked. [[sub-kernel-mmu]] "User page tables are charged and
reclaimed".
