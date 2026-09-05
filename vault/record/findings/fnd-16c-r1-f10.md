---
id: fnd-16c-r1-f10
type: fnd
title: "Pivot rights-gate comment wrong about RIGHT_WRITE"
round: adt-16c-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

The comment implied a write right was needed; pivot binds a name and
creates no edge.

## Disposition

Fixed: comment corrected.
