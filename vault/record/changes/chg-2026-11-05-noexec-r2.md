---
id: chg-2026-11-05-noexec-r2
type: chg
title: "T_E_NOEXEC: round 2's fixes, and an image refused by policy reads EACCES"
date: 2026-11-05
arc: arc-phase6-pouch
commits: [bb179781d, c32114c7c]
touched:
  - sub-kernel-exec
  - sub-substrate-build
established: [dec-2026-11-05-policy-refusal]
closed: [fnd-noexec-r2-f1, fnd-noexec-r2-f2, fnd-noexec-r2-f3, fnd-noexec-r2-f4, fnd-noexec-r2-f5]
opened: []
depth: skeletal
created: 2026-11-05
---
`adt-noexec-r2`'s fixes and operator vote 33. New: `execve.policy_refusal_says_eacces`, viv-pheno-probe L374, `/bin/exec-wx`, `/pheno-wx`, and the row "Exec's refusal errno" in `docs/AUDIT-TRIGGERS.md`.
