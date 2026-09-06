---
id: chg-2026-09-06-exec-internal-doc-absorb
type: chg
title: "absorb docs/reference/27-exec (P3-Eb kernel-internal exec / exec_setup): zero-fold, 6-surface redirect stub"
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
The kernel-internal exec reference (575 lines) -- exec_setup, the ELF->address-
space bridge, P3-Eb-era but updated through L-4a + D-4. The last big LINEAGE/exec
doc (completing 27+147+148). Verified atom-by-atom; every atom carried, zero fold
(sub-kernel-exec, warm from the 147-execve chunk, is comprehensive).

HOMES: the exec spine (the three exec_setup forms, the map-each-PT_LOAD sequence,
the sub-page floor geometry, L-4a sparse backing, the #107 I-cache span, the
argc/argv/envp/auxv frame, the D-4 PT_INTERP rewrite, the exec-with-threads guard)
-> sub-kernel-exec; elf_load + the W^X parse check -> sub-kernel-elf; the REVENANT
FILE fault arm (I-36) -> sub-kernel-fault; the Image cache + eviction safety ->
sub-kernel-image; the VMA + user-stack guard page -> sub-kernel-addrspace; the
BURROW mapping lifecycle (I-7) -> sub-kernel-burrow.

WHAT THE DOC GOT WRONG: its P3-Eb "kernel-internal only, EL0 transition is P3-Ed"
framing is long past (EL0 transition + SYS_SPAWN + SYS_EXECVE + REVENANT all
built); exec_setup is the spawn-into-empty-child path, distinct from execve's
DETACHED exec_load_into (L-2a) which this doc predates; the L-4a + D-4 updates it
carries are now the dossiers' content at more depth.

ZERO fold (the stale-milestone-superseded-by-current-dossiers vein). This
completes the LINEAGE/exec trilogy (27-exec + 147-execve + 148-fork all absorbed).
Render clean; lint 0-fail. view-absorption 77 -> 78.
