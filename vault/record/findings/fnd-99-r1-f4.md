---
id: fnd-99-r1-f4
type: fnd
title: "No deterministic test pinned the handler's errno return"
round: adt-99-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-dev9p]
threatens: []
fixed-by: chg-2026-07-19-99-create-errno
regression: go-fs 6c (O_EXCL EEXIST leg)
created: 2026-07-31
---
## Prosecution

The kernel test pins the accessor; go-fs 6b is schedule-dependent --
reverting the handler return would pass everything deterministic.

## Disposition

Fixed: go-fs step 6c -- a single-threaded O_CREATE|O_EXCL create of an
existing file must surface EEXIST via os.IsExist (reverting either half
surfaces EPERM -> IsExist false -> fails).
