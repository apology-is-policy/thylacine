---
id: chg-2026-09-06-burrow-absorb
type: chg
title: "docs/reference retirement: absorb 20-burrow -- a CROSS-LAYER absorption: fold A1/A4 into sub-kernel-burrow + A2 (the AEGIS teardown ordering) into sub-kernel-vma, then multi-redirect stub (53 absorbed / 104 live)"
date: 2026-09-06
arc: arc-vault
commits: ["6daac17b"]
touched: [sub-kernel-burrow, sub-kernel-vma]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
Completes the memory area (the last memory file), and the first CROSS-LAYER
absorption -- 20-burrow's atoms belong to three different dossiers, and pinning
each home was the work. Verified each placement in-tree before folding.

FOLDED into sub-kernel-burrow (its own missing API): A1, the deferred-free
burrow-side pair (burrow_release_mapping_deferred / burrow_free_deferred, the
may-sleep-so-defer reasoning, burrow_map_fixed) -- confirmed 0 hits before
folding; A4, that burrow_share_into maps the WHOLE region (no length param).

FOLDED into sub-kernel-vma (NOT burrow -- the code is vma_free's): A2, the unmap
PTE-teardown-before-free ordering -- mmu_uninstall_user_range + tlbi over the
range BEFORE the backing pages return to the buddy, the AEGIS-256/mallocng
stale-mapping corruption class it closes. Confirmed absent from vma/fault/mmu (the
one fault "hit" was the COW break, not this).

NOT folded (verified adequately homed): the handle.c KOBJ_BURROW release wiring
is in sub-kernel-handle (release-outside-lock-because-may-sleep, the consumed-ref
convention, TRANSFERABLE) + the dual-refcount interaction in sub-kernel-burrow;
A3 (per-page bit-47 reject in mmu_install_user_pte) is code-authoritative
defense-in-depth reduplicating the VMA-ceiling check burrow already documents.

Stub is MULTI-redirect (burrow primary + handle for the integration + vma for A2
+ the mmu code for A3). "What it got wrong": the doc's opening summary
contradicts its own six-type enum (ANON-only), and its share section is stale at
anon-only. No code touched; no audit owed. Both dossiers were already
updated:2026-09-06. view-absorption: 52 -> 53 absorbed, 104 live.

MEMORY AREA COMPLETE: asid, mmu, vma, addrspace, burrow, fault all absorbed
except 25-fault-dispatcher (the exception.c-sibling heavy one, still queued).
