---
id: fnd-stalk2-r1-f3
type: fnd
title: "Cross-mount `..` returns to the mount point, not Plan 9's mh parent"
round: adt-stalk2-r1
severity: P3
status: documented
surface: [sub-kernel-stalk]
threatens: []
created: 2026-08-01
---
## Prosecution

`..` at a crossed position pops back to the UN-crossed mount point
(which re-crosses on the next descent), not above it to the directory
the mount sits on — Plan 9 achieves the latter with persisted
`Chan->mh` back-pointers. I-28 containment is PRESERVED (`..` still
cannot escape `root_spoor`; depth-0 is a hard no-op); only the
cross-boundary DIRECTION differs.

## Disposition

Documented as the deliberate v1.x deferral (STALK-DESIGN §4.3): the
`mh` model lands with dirfd-persisted mount heads (and symlinks force
the related handle-based dot). Carried as a [[sub-kernel-stalk]] seam;
no action owed at v1.0.
