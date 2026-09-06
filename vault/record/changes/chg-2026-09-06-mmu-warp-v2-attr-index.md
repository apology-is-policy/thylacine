---
id: chg-2026-09-06-mmu-warp-v2-attr-index
type: chg
title: "sub-kernel-mmu de-stale: Warp-6 V-2 -- the PTE encoder's MAIR-index widening (NORMAL_NC), the W^X guard confined to NORMAL_WB, and the bool->index wrapper"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-mmu
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-kernel-mmu]] (updated 2026-08-16) predates Warp-6 V-2 `7973f8dc`
(2026-08-19), the only post-dossier commit on `mmu.c`/`mmu.h` -- the PTE-encoder
half of the host-visible BAR story (the pci_handle half went into
[[sub-kernel-hwcap]] and the burrow/fault half into [[sub-kernel-fault]] this
run). An `audit: hard`, W^X-critical surface ([[inv-i12]]). Verified in source.

- **The attribute axis generalized bool -> MAIR index.** `make_user_pte_l3` took
  `u32 mair_idx` in place of `bool device_memory`: `NORMAL_WB` (cacheable RAM,
  the default), `DEVICE` (nGnRnE, MMIO), and -- new at V-2 -- `NORMAL_NC`
  (write-combining) for host-visible shared memory, the attribute the bool could
  not express. (`NORMAL_NC` already existed in the MAIR since P1-C; V-2 plumbs
  the existing index, so the MAIR setup itself is unchanged.)
- **The W^X extinction WIDENED (I-12).** The old guard extincted on
  execute-plus-device; the new one extincts unless the executable mapping's index
  is `NORMAL_WB` -- so execute on Device AND execute on the new NC index both
  hard-fail at the encoder. An executable page is only ever legitimate on
  cacheable Normal-WB RAM; this is the encoder-level backstop beneath the VMA
  gate that already forbids W^X. Plus a range guard: an out-of-range MAIR index
  extincts rather than selecting an unimplemented attribute or overflowing the
  3-bit AttrIndx field.
- **The bool install API is now a wrapper.** `mmu_install_user_pte(bool)` maps
  false->WB / true->DEVICE over the index-aware `mmu_install_user_pte_attr`, so
  the bool->index mapping lives in ONE place -- a bare `false` and
  `MAIR_IDX_DEVICE` share the bit pattern 0, so a raw-index-only API would invite
  that fat-finger.

Folded into the Mechanism PTE-encoder paragraph, the Error-paths extinction list
(the stale "execute-on-device at the encoder" corrected), and a Provenance
re-read. `updated:` -> 2026-09-06. Stale backlog 30 -> 29.
