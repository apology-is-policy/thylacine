---
id: fnd-walk-errno-r1-f4
type: fnd
round: adt-walk-errno-r1
severity: P3
status: fixed
title: "ERRORS.md did not name what the kernel originates, and EBADF misnamed a fidless walk"
surface: [sub-kernel-ninep-dev9p]
threatens: []
fixed-by: chg-2026-11-03-walk-errno
regression: "dev9p: a walk from a fidless Spoor is no miss"
created: 2026-11-03
---
## Prosecution

The row named the server's errno, a dead session and an OOM, and the chunk also originated EBADF, EMFILE, EINVAL and an over-count EIO. Fixed: the row names them; a walk from an open fid reads EINVAL, Stratum's own word for it. Not reachable from EL0.
