---
id: fnd-noexec-r2-f2
type: fnd
round: adt-noexec-r2
severity: P3
status: fixed
title: "No audit row named the surfaces this chunk added"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-05-noexec-r2
regression: null
created: 2026-11-05
---
## Prosecution

`docs/AUDIT-TRIGGERS.md` had no mention of `T_E_NOEXEC`, `exec_image_check` or `vma_args_ok`, though the chunk adds a parent-side read before the rfork and moves an I-12 check out of the one constructor.

## Disposition

Row and index line added: "Exec's refusal errno".
