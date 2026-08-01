---
id: chg-2026-05-25-16b-beta-hw-openat
type: chg
title: "16b-beta: the HW-capability numbers + the first path-based open"
date: 2026-05-25
arc: arc-pouch-boot
commits: ["fc28aaa3"]
touched:
  - sub-pouch-seam
  - sub-pouch-fs
established: []
closed: []
opened: ["seam-pouch-dirfd"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
Two patches for one goal -- getting stratumd to its keyfile. 0008
exposes the six `CAP_HW_CREATE` syscall numbers at the musl ABI so
Stratum's in-process virtio-blk driver can drive the disk with raw
`syscall()` calls. 0009 rewrites `openat` to walk an absolute path one
component at a time via `SYS_WALK_OPEN`, lifting the "path-based open is
v1.x" line because `stm_keyfile_load` needs it.

The walk loop opened every intermediate with the FINAL omode -- a design
error that stayed invisible for two months, until PTY-3's
`posix_openpt` needed to open a write-mode file across two mounts.
