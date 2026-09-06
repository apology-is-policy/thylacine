---
id: chg-2026-09-06-revenant-doc-absorb
type: chg
title: "absorb docs/reference/126-revenant (REVENANT file-backed demand-paged exec, I-36): zero-fold, 6-surface redirect stub"
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
The REVENANT reference (505 lines) -- an I-36 AUDIT-TRIGGER surface (file-backed
demand-paged exec, the Plan 9 Image model as BURROW_TYPE_FILE). Verified the
seven I-36 conditions atom-by-atom against six fresh owners (warm from 27-exec +
the fault #137 fold); every condition enforced-and-homed, zero fold.

HOMES (the seven I-36 conditions): R-1 burrow_create_file (sparse filepages, the
I-30 pinned Spoor, burrow_free_deferred) -> sub-kernel-burrow; R-2/R-5/R-6 the
demand-page fault arm (file_fault_req sleeping + the lock-break protocol, #811
death-interruptible, FAULT_USER_BUS/snare:bus fail-closed) -> sub-kernel-fault;
R-3 the Image cache (image_lookup_or_create, the seven-field key whose qid.version
makes coherence free, the #194 backing-size stamp = condition 7, the
eviction-cannot-race-a-mapper proof) -> sub-kernel-image; R-4 exec_setup_from_spoor
+ exec_resolve_from_namespace -> sub-kernel-exec; R-4 arch_icache_sync_range (#317)
-> sub-kernel-mmu; condition 3 W^X (elf_load PF_W|PF_X reject) + condition 4 the
eager-copy -> sub-kernel-elf + sub-kernel-addrspace. Conditions 1-2 come free from
Stratum's Merkle FS.

WHAT THE DOC GOT WRONG: little -- as-built I-36 reference, current (carries #45,
#149, #194/D-3c past-EOF). The change is distribution across 6 dossiers, each at
more depth (the Image-cache eviction proof + the fault-arm lock-break protocol are
the load-bearing arguments). The per-page I-32 charge for shared text stays a v1.x
refinement (both the doc and sub-kernel-fault record it).

ZERO fold. This completes the REVENANT/I-36 arc (alongside 27-exec). Render clean;
lint 0-fail. view-absorption 78 -> 79.
