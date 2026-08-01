---
id: fnd-rw4-rev2-f2
type: fnd
title: "RW-4 R2-F2: stat_native-implies-seekable regressed lseek on devsrv/devproc"
round: adt-rw4-r1
severity: P3
status: fixed
surface: [sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-06-10-rw4-fixes
created: 2026-08-01
---
## Prosecution

The lseek gate used `stat_native != NULL` as its seekability
heuristic. #957 (devsrv) and A-4b (devproc) added `.stat_native` for
fstat on NON-seekable Devs — so lseek began succeeding on an ignored
offset for both.

## Disposition

Fixed: an explicit `dev->seekable` flag (true on devramfs + dev9p
only); the gate is now `!dev->seekable`. The decoupling later became a
standing prosecution item on every new `.stat_native` (the RW-4 R2-F2
line cited by the #96/#97 pipe/notes stat additions: adding stat must
NOT re-enable lseek).
