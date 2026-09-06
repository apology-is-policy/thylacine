---
id: chg-2026-09-06-legate-doc-absorb
type: chg
title: "absorb docs/reference/102-legate (I-25 legate elevation): zero-fold, multi-redirect"
date: 2026-09-06
arc: arc-vault
commits: ["37a261fb"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/102-legate.md -> ABSORBED (I-25)

Absorbed the 385-line legate reference doc into a multi-redirect stub. Verified
covered: the legate kernel mechanism (CAP_ELEVATION_ONLY strip, CAP_GRANT_CLEARANCE,
proc_become_legate, the cap device wire, the lifecycle, I-25) -> sub-kernel-caps
(whose title names the legate; updated 2026-09-05); the corvus clearance subsystem
+ the A-4a-3 verbs (ADMIN_ELEVATE/RECOVER, the activation path that mints a legate)
-> sub-corvus; the perm_check axis the elevated caps feed -> sub-kernel-perm.

Zero fold. The cross-referenced docs (75-devcap, 95-identity, 99-fs-permission)
are themselves absorbed/absorbing into sub-kernel-caps/perm.

95 -> 96 absorbed of 157. lint 0-fail.
