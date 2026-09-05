---
id: chg-2026-05-28-fs-beta
type: chg
title: "FS-beta: SYS_FSYNC + SYS_READDIR (durability + enumeration)"
date: 2026-05-28
arc: arc-identity-detour
commits: ["8feb187d"]
touched: [sub-kernel-ninep-dev9p]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
`dev9p_fsync` (Tsync barrier) + `dev9p_readdir` (Treaddir; the opaque-
cookie contract hardened later at #955) + the two vtable slots + syscalls.
Audited with FS-alpha (`2081408d`).
