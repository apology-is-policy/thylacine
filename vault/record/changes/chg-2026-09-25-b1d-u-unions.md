---
id: chg-2026-09-25-b1d-u-unions
type: chg
title: "B-1d-u (Plan 9 unions): an MBEFORE or MAFTER mount at a bare directory keeps the directory it covers as a member; the spec's covered-directory invariants; the Rust target leaves static-PIE"
date: 2026-09-25
arc: arc-boosty
commits: ["3b52d769"]
touched:
  - sub-kernel-territory
  - sub-kernel-stalk
  - spec-territory
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-25
---
Plan 9 unions keep the directory they cover
([[dec-2026-09-24-union-covered-directory]]). An MBEFORE or MAFTER mount at a
directory that hosts no member records that directory beside the source, in
the order the flag names, as a member flagged `MCOVERED` (kernel-internal:
SYS_MOUNT's mask refuses it). The resolver walks and lists through it like any
member and refuses a create there without MCREATE; unmount never names it and
drops it with the last mounted member; a file point stays a plain mount. The
spec gains the covered directories, the `unioned` history and the UM-8
reposition, six invariants with a buggy configuration each, and a clean alias
configuration; [[spec-territory]] carries the TLC figures. The Rust target's
static-PIE flag goes false in the same commit, because the shared LLVM fork
already refuses `-static-pie` and main could not bake r1hello until it landed.
Audited with B-1d ([[adt-b1d-r1]], scope E).
