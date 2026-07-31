---
id: fnd-16c-r2-f5
type: fnd
title: "Dual-destroy magic distinctness unpinned"
round: adt-16c-r2
severity: P3
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

The dual-destroy's correctness rests on the two adapter magics being
distinct and at offset 0 -- enforced only by manual coordination; a future
adapter reusing a magic silently double-destroys.

## Disposition

Fixed: `_Static_assert` pair in 9p_attach.c (magics distinct; magic at
offset 0 in both types) -- still present and load-bearing today.
