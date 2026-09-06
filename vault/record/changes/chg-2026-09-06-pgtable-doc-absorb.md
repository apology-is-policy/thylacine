---
id: chg-2026-09-06-pgtable-doc-absorb
type: chg
title: "absorb docs/reference/24-per-proc-pgtable (P3-B allocator): zero-fold, 5-surface redirect stub; the dossiers are current and the doc's open trip-hazard is closed"
date: 2026-09-06
arc: arc-vault
commits: ["767181bf"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The per-Proc TTBR0 page-table allocator reference (261 lines), Phase-3-era.
Verified atom-by-atom against five owners; every atom carried, zero fold, and two
of the doc's claims turned out stale in the good way (a hazard it flagged is
closed).

HOMES: proc_pgtable_create/_destroy (recursive L0->L3 free, #116) +
mmu_install_user_pte (walk-grow-install, OOM, idempotent) + the PTE encoding
(PXN/UXN/AP/nG, W^X by construction) -> sub-kernel-mmu (fresh, owns arch/arm64/
mmu.c). pgtable_root field + lifecycle -> sub-kernel-addrspace (it moved onto the
AddrSpace at L-4). TTBR0 = (asid<<48)|pgtable_root install at cpu_switch_context +
sched_activate_addrspace -> sub-kernel-sched-smp. The address-space-lock
serialization of the walk -> sub-kernel-fault. The teardown TLB lifecycle
(ASID-tag + asid_free tlbi) -> sub-kernel-asid.

VERIFY-BEFORE-FOLD: passed my own false path (I fed quaestor arch/arm64/pgtable.c;
it reported UNOWNED because the file DOESN'T EXIST -- an artifact of my argument,
not a coverage gap; the allocator lives in mmu.c, grep-confirmed). Then confirmed
the mmu dossier's Concurrency section (L137-140) serializes the per-Proc walk
under the address-space lock, which CLOSES the multi-thread race the doc flagged
as an open trip-hazard.

WHAT THE DOC GOT WRONG (named in the stub): (a) pgtable_root is on the AddrSpace,
not the Proc (LINEAGE L-4 -- what lets RFMEM siblings + peer threads share one
address space); the allocator fns are unchanged, their OWNER moved; (b) the
multi-thread threading hazard is CLOSED, not an open per-Proc-lock TODO -- the
demand-fault path holds lock-vma across resolve-and-install; (c) the Phase-3
forward-references ("P3-Bd will wire", "P3-Dc's fault handler", "future caller")
all landed.

ZERO fold: the 19-handles/21-elf/08-exception "stale milestone doc superseded by
current dossiers" vein. Render clean; lint 0-fail. view-absorption 74 -> 75.
