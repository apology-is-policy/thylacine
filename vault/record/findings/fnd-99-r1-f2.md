---
id: fnd-99-r1-f2
type: fnd
title: "mkdir failure arm did not latch fid_suspect"
round: adt-99-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: []
fixed-by: chg-2026-07-19-99-create-errno
created: 2026-07-31
---
## Prosecution

Asymmetric with the lcreate arm (the G2 backstop contract): a mkdir-only
workload could re-park a stale fid.

## Disposition

Fixed: fid_suspect latched in the mkdir rc != 0 arm.
