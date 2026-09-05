---
id: fnd-16c-r2-f6
type: fnd
title: "Auto-arm fires even with data already in the ring"
round: adt-16c-r2
severity: P3
status: withdrawn
surface: [sub-kernel-ninep-transport]
threatens: []
created: 2026-07-31
---
## Prosecution

The auto-arm armed a deadline even when the recv would complete
immediately from buffered bytes.

## Disposition

Withdrawn: MOOTED by the F1R2 gate refinement at the round (and doubly
mooted by #841's removal of the auto-arm altogether).
