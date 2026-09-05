---
id: fnd-99-r1-f3
type: fnd
title: "Dir-create mid-sequence failures collapsed to the generic -1"
round: adt-99-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: []
fixed-by: chg-2026-07-19-99-create-errno
created: 2026-07-31
---
## Prosecution

Fid-pool exhaustion / walk-to-child / lopen failures after a successful
Tmkdir reported an opaque -1 for a directory that WAS created.

## Disposition

Fixed: real errnos recorded per arm (-T_E_NOMEM / rc / -T_E_IO) +
fid_suspect where a by-name op erred.
