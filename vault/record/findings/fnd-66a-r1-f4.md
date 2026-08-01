---
id: fnd-66a-r1-f4
type: fnd
title: "#66a F4: confined-Proc namespace-layout disclosure via retained names"
round: adt-66a-r1
severity: P3
status: fixed
surface: [sub-kernel-path]
threatens: []
fixed-by: chg-2026-06-12-66a-spoor-path
created: 2026-08-01
---
## Prosecution

Chroot name residue + inherited-fd names let a confined Proc
`fd2path` the OUTER namespace layout (e.g. `/var/lib/corvus`, a
container host prefix). NO authority impact — I-33 holds, `..` stays
contained — but it is namespace-STRUCTURE information crossing a
confinement boundary.

## Disposition

Fixed (doc/framing): ARCH §9.6.9 now frames the v1.x re-stamp-at-chroot
as a *disclosure* fix, not cosmetics, and the fd2path comment was
corrected to "already holds / spawner walked". Carried as the
[[sub-kernel-path]] seam; the mechanism change itself is deliberately
deferred.
