---
id: fnd-noexec-r1-f7
type: fnd
round: adt-noexec-r1
severity: P3
status: fixed
title: "Two kernel inconsistency arms read as the file's fault"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-04-noexec-r1
created: 2026-11-04
---
## Prosecution

A NULL lazy slot and a misaligned header buffer flattened to `-1`, and `-1` from a mapper is NOEXEC.

## Disposition

Both read `-T_E_IO`. Neither is reachable without a kernel defect, so no witness; `exec_image_check`'s unreachable arm reads `-T_E_ACCES`, the resolver's word for the same state.
