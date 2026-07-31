---
id: fnd-16c-r2-f4
type: fnd
title: "R1-F5's close justification was inaccurate"
round: adt-16c-r2
severity: P3
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

The R1 record claimed the dual-destroy's cast pointer is never
dereferenced; it IS read+written through.

## Disposition

Fixed: the justification replaced with the real mechanism (offset-0 magic
read is layout-compatible; the magic check short-circuits the wrong-type
path) -- a closed-list entry corrected by a later round, the record
correcting the record.
