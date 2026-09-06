---
id: chg-2026-09-06-overcommit-doc-absorb
type: chg
title: "absorb docs/reference/127-overcommit (lazy-anon demand-zero + decommit, I-32 fourth axis): zero-fold, 4-surface redirect stub"
date: 2026-09-06
arc: arc-vault
commits: ["31841eed"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
The overcommit reference (255 lines, #319) -- lazy-anon demand-zero + decommit,
the I-32 fourth axis (a live-VMA cap). Verified atom-by-atom across four fresh
owners (warm from the addrspace page-budget work); zero fold.

HOMES: burrow_create_anon_lazy (the substrate) + burrow_decommit (release resident
pages without unmapping) -> sub-kernel-burrow; the demand-zero fault arm
(allocate+zero+install-once under the lock, no slow path) + charge-on-fault
(page_count = true RSS, charged BEFORE the allocation fail-closed) -> sub-kernel-
fault; the I-32 accounting + the vma_count fourth axis + the atomic charge
discipline -> sub-kernel-addrspace; the lazy-attach syscall + decommit ABI ->
sub-kernel-syscall-abi + sub-kernel-syscall-dispatch.

WHAT THE DOC GOT WRONG: the userspace malloc-substrate wiring "lands at #321" is
built (libthyla-rs sysAlloc, pouch mmap boundary-line, Go sysReserve/sysUnused);
the content is now distributed across 4 dossiers, each at more depth (the charge
concurrency + the fail-closed charge-before-allocate ordering, the I-32 soundness
arguments).

ZERO fold. Render clean; lint 0-fail. view-absorption 79 -> 80.
