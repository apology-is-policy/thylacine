---
id: chg-2026-09-06-fault-distro-hostmem
type: chg
title: "kernel-fault de-stale: the seventh (HOSTMEM) backing arm + the MAIR-index install generalization (Warp-6 V-2), the #190 geometry verify-and-bail (D-3 retires the R-5 F2 premise, and F2's recompute remedy was wrong), and the #194 past-EOF SIGBUS"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-kernel-fault
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-06
---
[[sub-kernel-fault]] read `updated: 2026-08-16`; three commits `7e89a3b6..HEAD`
(+100/-29 on `arch/arm64/fault.c`) landed after it. Ground-truthed by reading
the diff and the current handler. Three real additions folded:

- **Warp-6 V-2: the SEVENTH backing arm (HOSTMEM).** A PCI hostmem BAR subrange:
  physical backing like MMIO/DMA (`pa + page offset`), but the install attribute
  is the create-time HOST-DICTATED MAIR index (`hostmem_mair`: NORMAL_WB CACHED /
  NORMAL_NC WC), honoured exactly, `kobj_pci` non-NULL the liveness guard. That
  forced the `device_memory` bool (a two-way Device/Normal choice) to widen to a
  full `mair_idx` installed through `mmu_install_user_pte_attr`; the other six arms
  pass a fixed index and are byte-identical. The dossier's arm COUNT drifted a
  THIRD time (5 -> 6 -> 7) -- the exact miscount shape it warns about; corrected in
  the heading, the title, and the table (a HOSTMEM row).
- **#190: the file arm's step 3 now VERIFIES geometry, not just identity.** D-3
  retired the R-5 F2 premise ("a FILE Burrow is created only by exec, mapped once
  at offset 0") on every count -- EL0 file mmap, a MAP_FIXED split giving a tail a
  non-zero `burrow_offset`, one Image Burrow across several address spaces -- so
  "same Burrow?" answers yes to a split and no longer suffices. The check now
  re-proves `page_va in [start,end)` AND `slot_now == freq->slot` and BAILS to a
  re-fault on a mismatch (both single + cluster paths). F2's PRESCRIBED remedy
  ("recompute the slot") was WRONG: `file_offset` was also pre-sleep-derived and
  the read page holds its bytes, so a recompute files stale bytes under a fresh
  slot -- the same corruption, tidier address. Verify, never recompute.
- **#194: a fault wholly past the file's last page is `FAULT_USER_BUS`, not a
  zero-fill.** Demand-zeroing it mints memory the I-32 axis never sees (FILE
  accounting justified by SHARED FILE BYTES, which a past-EOF page is not); refused
  BEFORE any allocation. The final PARTIAL page still zero-fills (Linux fidelity).
  Read-ahead carries the same bound -- a past-limit neighbour would become a
  resident zero page the fast path installs uncharged.

Folded into the title, the backing-arms count + table + a new install-attr
paragraph, Concurrency (step 3 + the #190 rationale), Invariants (I-36
generalized to phenotype mmap), Error paths (#194), Seams (the chosen-offset seam
DISCHARGED), Provenance. `updated:` -> 2026-09-06. Stale backlog 39 -> 38.
