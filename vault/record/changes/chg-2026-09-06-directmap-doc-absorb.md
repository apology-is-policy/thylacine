---
id: chg-2026-09-06-directmap-doc-absorb
type: chg
title: "absorb docs/reference/23-direct-map (P3-Bb kernel direct map): zero-fold, 3-surface redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The Phase-3 kernel direct-map reference (200 lines), foundational-refactor era.
Verified atom-by-atom across three owners; every atom carried, zero fold.

HOMES: the direct-map + vmalloc mapping machinery (TTBR1 high-half layout, the
linear PA->KVA map, vmalloc range, identity-map retirement, PTE attrs, alias
semantics, TLB visibility) -> sub-kernel-mmu; pa_to_kva/kva_to_pa + the l1_directmap
cap (the #808 absolute-cap seam/finding the doc predates) -> sub-kernel-mm-phys;
the SLUB slab-pointers-in-the-direct-map consumer -> sub-kernel-mm-slub.

WHAT THE DOC GOT WRONG: its P3-Bb forward-refs all landed (per-Proc TTBR0, the
swap, demand paging -- see 24-per-proc-pgtable, itself absorbed); and it predates
#808, so it carries no warning that the l1_directmap cap is ABSOLUTE not relative
to mem_base (a bringup at mem_base != 1 GiB would deref past the window) --
sub-kernel-mm-phys carries the seam.

ZERO fold (the stale-milestone-superseded-by-current-dossiers vein). Render clean;
lint 0-fail. view-absorption 75 -> 76.
