---
id: fnd-16c-r1-f6
type: fnd
title: "SYS_PIVOT_ROOT vs concurrent walk from root_spoor (multi-thread)"
round: adt-16c-r1
severity: P2
status: deferred
surface: [sub-kernel-ninep-attach]
threatens: []
seam: seam-848-pivot-walk-race
created: 2026-07-31
---
## Prosecution

A multi-thread Proc pivoting while a sibling resolves from `root_spoor`
reads a torn pointer / freed Spoor. Inherited from `territory_chroot`'s
identical pattern -- not specific to 16c.

## Disposition

Deferred at the round to the Phase-5+ multi-thread carve-out (dormant:
joey pivots single-threaded during bringup). Re-confirmed at the #844
handle-lifetime pass (its F3 tracked it as #848). Carried as the linked
seam; re-homes to the territory dossier at that sweep.
