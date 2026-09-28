---
id: fnd-b1a-prime-r2-f8
type: fnd
title: "The Image cache is a pool hoard nobody pays for: a FILE page-in is pool-charged but counted against no address space, survives its toucher's death as an idle entry, and nothing reclaims it until 120 more images evict it"
round: adt-b1a-prime-r2
severity: P2
status: fixed
surface: [sub-kernel-image, sub-kernel-mm-phys, sub-kernel-fault, sub-kernel-vma, sub-kernel-addrspace, sub-kernel-burrow]
threatens: [inv-i32]
fixed-by: chg-2026-09-23-b1a-prime-close-r2
regression: "demand_page.file_pages_charge_the_holder, demand_page.idle_image_reclaimed_under_pressure"
created: 2026-09-23
---
## Prosecution

`sys_mmap_file` admits `BURROW_ATTACH_MAX` per call at any offset and
`image_lookup_or_create` keys on (qid, offset, size, exec), so k windows of
one file are k cached Burrows -- 128 slots by 256 MiB is 32 GiB of admissible
hoard. The FILE miss allocates `alloc_user_pages(0, 0, exempt)` -- pool-charged
-- and installs with `pagemap_install(.., as = NULL, ..)`: neither the page nor
its nodes touch the toucher's `page_count`. The toucher exits; its drain drops
the mapping refs only, leaving the cache's entry at {1, 0}; `find_install_slot`
evicts an idle entry only when all 128 slots are used, and `pool_charge` has
no reclaim step. With the attacker dead, `charged ~= pool`: every non-exempt
`alloc_user_pages` machine-wide is refused (a demand-zero touch terminates its
Proc) while gigabytes of idle cache exist. Also a confinement bypass: a
narrowed cap never sees file pages. And `mm/phys.h` placed the Image cache in
the RESERVE, where it is pool memory. Attribution: the uncharged FILE posture
(R-5) predates the chunk and was worse, unbounded; the physical pool converted
"eat all RAM" into "hold the whole pool forever".

## Fix

Two halves ([[sub-kernel-image]] "The cache is the pool's reserve under
pressure"; [[sub-kernel-mm-phys]] "The pool reclaims before it refuses").
The reclaim: `capacity_set_reclaim(fn)` registers one function with the
allocator, and `alloc_user_pages` asks it for the pages it needs each time
`pool_charge` refuses a non-exempt charge, then asks the pool again;
`image_cache_reclaim` strips, under `g_image_lock`, the least recently used
idle entries (one handle, no mapping, resident pages) of their pages
(`burrow_image_strip`: `pagemap_take_next` by resident slot, frees outside
`v->lock`, no COW put) and leaves them cached and empty for the next mapper to
page in again -- the {1, 0} idleness the eviction proof already established
under that lock is what makes freeing a page nothing else names safe. The
holder charge: every FILE leaf install is charged to the mapping space
(`addrspace_charge_file`: `page_count` + a `file_pages` telemetry; refunded
when the install finds the leaf already there) and refunded per leaf a range
clear removes (`vma_uninstall_range_in`, the range clear mapping by mapping,
now the one clear every teardown uses); `/proc/<pid>/status` prints `file:`
and the capacity probe subtracts it. The `phys.h` prose now says the reserve
is kernel memory and the Image cache is pool memory reclaimed under pressure.
