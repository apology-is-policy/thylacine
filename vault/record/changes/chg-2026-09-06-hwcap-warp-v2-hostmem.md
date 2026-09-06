---
id: chg-2026-09-06-hwcap-warp-v2-hostmem
type: chg
title: "sub-kernel-hwcap de-stale: Warp-6 V-2 -- the host-visible BAR map authority (SYS_BURROW_FROM_HOSTMEM), the hostmem_burrows counter, and the DMA-only owner-death quiesce (F1)"
date: 2026-09-06
arc: arc-vault
commits: ["b5240804"]
touched:
  - sub-kernel-hwcap
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-kernel-hwcap]] (updated 2026-08-16) predates Warp-6 V-2 `7973f8dc`
(2026-08-19), the only post-dossier commit on `pci_handle.{c,h}` -- the KObj_PCI
half of the host-visible BAR story whose fault/burrow side went into
[[sub-kernel-fault]] last run. Verified in the current source. An `audit: hard`
surface (I-5/I-32/I-34, and now I-45 on the map subrange).

- **The map authority** (Contract + shared-memory-regions). Owning a bus-function
  claim now confers a further authority: `SYS_BURROW_FROM_HOSTMEM` maps a subrange
  of one of its discovered shared-memory BARs into a client VA as a physical-backed
  `BURROW_TYPE_HOSTMEM` Burrow. No extra capability -- the handle is [[inv-i5]]
  non-transferable, so holding it IS the authority (no `CAP_HW_CREATE`) -- and the
  subrange is contained inside the discovered window by the same non-wrapping
  arithmetic the discovery walk uses ([[inv-i45]]; the physical base never escapes
  the BAR).
- **`hostmem_burrows` + the DMA-only death quiesce** (Teardown + Data structures +
  Prosecution; the audit F1 [P1]). A cross-Proc hostmem mapping means owner death
  cannot simply disable MEM-decode -- that would yank a live mapping from another
  Proc. So KObj_PCI counts live hostmem Burrows (`hostmem_burrows`, bumped at
  create, dropped at free) and, for a claim with any, does a DMA-only quiesce
  (`kobj_pci_quiesce_dma_only`: BUS_MASTER cleared so the dead device stops
  writing, MEM_SPACE KEPT so the client's BAR keeps answering). Full MEM-decode
  disable is deferred to the last `kobj_pci_unref`. A new Prosecution bullet pins
  the count-accuracy hazard (undercount -> decode-disabled BAR under a live mapping;
  overcount -> BAR pinned forever).

Folded into Contract, the shared-memory-regions Mechanism paragraph, the Teardown
paragraph, Data structures (the function object's field list), Prosecution, and a
Provenance re-read entry. `updated:` -> 2026-09-06. Stale backlog 31 -> 30.
