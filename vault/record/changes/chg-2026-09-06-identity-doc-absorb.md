---
id: chg-2026-09-06-identity-doc-absorb
type: chg
title: "absorb docs/reference/95-identity (identity model A-1a, I-22): zero-fold, 5-surface redirect stub"
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
The identity-model reference (253 lines, A-1a). Verified atom-by-atom across five
fresh owners; zero fold.

HOMES: the identity fields (principal_id/primary_gid/supp_gids) + inheritance +
kproc=SYSTEM + proc_apply_identity (the single audited mutation site, extinct-on-
stamp of PRINCIPAL_SYSTEM/INVALID) -> sub-kernel-proc; CAP_SET_IDENTITY + I-22
(capabilities-only growth) -> sub-kernel-caps; the race-free property
(proc_apply_identity runs in the child before EL0) + I-22 enforcement -> sub-kernel-
perm; srv_peer_info/SYS_SRV_PEER/SO_PEERCRED -> sub-kernel-ninep-attach; the spawn
ABI identity block + srv_peer_info record -> sub-kernel-syscall-abi.

WHAT THE DOC GOT WRONG: "A-1b (corvus identity DB + RESOLVE_* + CRVS v2) not yet
landed" is stale -- corvus is built (its dossiers were absorbed earlier this
sweep). Content distributed across 5 dossiers.

ZERO fold. Render clean; lint 0-fail. view-absorption 83 -> 84.
