---
id: chg-2026-11-02-b3h-audit-r1-fixes
type: chg
title: "B-3h audit round 1's fixes"
date: 2026-11-02
arc: arc-boosty
commits: ["848ffd738"]
touched:
  - sub-kernel-filelock
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-session
  - sub-kernel-handle
  - sub-kernel-dev
  - sub-kernel-uaccess
  - sub-kernel-syscall-dispatch
  - sub-kernel-syscall-abi
  - sub-kernel-stalk
  - sub-kernel-ninep-attach
  - sub-pouch-seam
  - sub-pouch-fs
  - sub-stratum-boot
established: []
closed: []
opened: []
mirrors-checked: [docs/ARCHITECTURE.md, docs/FID-LIFECYCLE-DESIGN.md, docs/manual/22-file-locks.md, docs/AUDIT-TRIGGERS.md]
depth: skeletal
created: 2026-11-02
---
Audit round 1 on B-3h returned 1 P0, 2 P1, 2 P2 and 12 P3 ([[dec-2026-10-29-b3h-file-locks]]).
A `SYS_FILE_LOCK` test copied its answer to an unchecked address, so any program
could halt the kernel with a kernel-half pointer; the range is now checked before
the server is asked. A write lock converted to a read lock released it before the
staged writes landed, and two tests had pinned that as correct: every set-lock now
flushes first, as Linux's NFS client does. A loose cached open's first `wstat` was
judged by its open-time owner, so a former owner could change a file; `wstat_apply`
now binds first through the Dev's `wstat_bind`. An `O_PATH` close dropped the Proc's
locks, and `chmod(path)` closes one inside the call; it no longer does. Every wire
failure in a loose open's re-walk read as ESTALE, which let a take stand while a peer
kept serving its snapshot; only a name that no longer reaches the file is ESTALE now.
The P3s fixed here: a mint's clone errno, a close hook that could unlock another file
after an identity failure, a range reaching 2^64, a loose mint from an opened base that
could never bind, `proc_id` naming the fid, flock's op order, and `SEEK_CUR` on a pipe.
The docs now say an open-file take also has every open of the file in the Proc read the
server, which the code has done on purpose. F13 (Stratum's per-file-system record cap
is a first-come DoS) and F15 (a file keeps its slot in the Proc's table after an unlock,
now in the manual) stay open in the bug queue. Round 2 is owed on these fixes.
