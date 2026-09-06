---
id: chg-2026-09-06-fault-absorb
type: chg
title: "docs/reference retirement: absorb 25-fault-dispatcher -- fold C2/C3/C4 (fault's own demand-page properties) into sub-kernel-fault, dual-redirect with the exception sibling; MEMORY AREA COMPLETE (54 absorbed / 103 live)"
date: 2026-09-06
arc: arc-vault
commits: ["c6eed249"]
touched: [sub-kernel-fault]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The last memory-area file, and it COMPLETES the area (mmu, asid, vma, addrspace,
burrow, fault all absorbed). The Explore flagged it "heaviest" for the
exception.c sibling, but the three content atoms turned out to be fault's OWN
(single-dossier), not cross-layer -- verified before folding:

FOLDED into sub-kernel-fault: C2 (an install into a not-present leaf needs no
TLB invalidation, invalid->valid per ARM ARM B2.7.1; the fast path never
overwrites a valid leaf, so it issues no TLBI -- 0 hits before folding), C3 (the
ordinary anon/lazy arms take NO Burrow ref -- the page is a PA into the VMA's
already-mapped Burrow whose mapping_count is the liveness, so NoUseAfterFree
holds by construction; only the file arm, which drops the lock to sleep, pins --
the 3 pre-fold hits were the file-arm pin, the opposite case), C4 (the offset is
bounded: burrow_byte_off rejected if >= burrow->size -- 0 hits).

SIBLING (verified, 8 hits): the EL0 sync-vector dispatch layer (arch/arm64/
exception.c) is covered by sub-kernel-exception -> DUAL-redirect stub (fault +
exception), no orphan.

Stubbed with honest drift: the doc claims single-threaded-needs-a-lock (superseded
by #713 vma_lock), carries only 3 fault_result values (dossier has 4 incl.
FAULT_USER_BUS), and predates the seven arms / HOSTMEM / D-3 / COW.

No code touched; no audit owed. sub-kernel-fault already updated:2026-09-06.
view-absorption: 53 -> 54 absorbed, 103 live. MEMORY AREA 100% ABSORBED.
