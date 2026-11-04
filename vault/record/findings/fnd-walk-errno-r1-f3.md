---
id: fnd-walk-errno-r1-f3
type: fnd
round: adt-walk-errno-r1
severity: P3
status: fixed
title: "A server's ENOSYS to a plain walk read as a missing translator"
surface: [sub-kernel-ninep-dev9p]
threatens: []
fixed-by: chg-2026-11-03-walk-errno
regression: "dev9p.walk_enosys_reads_eio"
created: 2026-11-03
---
## Prosecution

A Linux guest's T2 row reads ENOSYS as its own decline and reports the row unserved. Fixed: dev9p_walk folds a server's ENOSYS and EOPNOTSUPP to EIO; the fused op's two are its latch.
