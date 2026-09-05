---
id: fnd-16c-r1-f13
type: fnd
title: "territory_pivot_root duplicates territory_chroot's body"
round: adt-16c-r1
severity: P3
status: documented
surface: [sub-kernel-ninep-attach]
threatens: []
created: 2026-07-31
---
## Prosecution

Two copies of the post-precondition swap logic drift independently.

## Disposition

Documented in the function header; the shared-helper refactor is v1.x
cleanup, not load-bearing. No seam (hygiene).
