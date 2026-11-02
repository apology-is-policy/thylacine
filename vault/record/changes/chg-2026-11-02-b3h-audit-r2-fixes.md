---
id: chg-2026-11-02-b3h-audit-r2-fixes
type: chg
title: "B-3h audit round 2's fixes"
date: 2026-11-02
arc: arc-boosty
commits: ["*(pending)*"]
touched:
  - sub-kernel-filelock
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-session
  - sub-kernel-ninep-client
  - sub-kernel-ninep-wire
  - sub-kernel-handle
  - sub-kernel-dev
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-pouch-fs
established: []
closed: []
opened: []
mirrors-checked: [docs/ARCHITECTURE.md, docs/FID-LIFECYCLE-DESIGN.md, docs/ERRORS.md, docs/manual/21-environment.md, docs/manual/22-file-locks.md]
depth: skeletal
created: 2026-11-02
---
Audit round 2 on B-3h returned 1 P0, 1 P2 and 9 P3 ([[dec-2026-10-29-b3h-file-locks]]).
The P0 predates B-3h, from POUNCE (2026-07-07), and is live on `main`: the session
capped a walk reply's count at `P9_MAX_WALK` rather than at what the walk asked, so a
server, which any process can be over two pipes, wrote past a one-element kernel array
on the heap or the stack. Both arms now parse with the op's own count, and a longer
reply ends the session. The P2: a close that released a lock returned before the
release was ordered, since a clunk never waits and Stratum's pool may serve the next
request first; a fid a lock was granted on now unlocks and waits, then clunks. The P3s:
`SEEK_CUR` on a real pipe answered EIO, since the kernel says "no cursor" with a flat -1
which `__lseek` reports as EIO; a bind now installs its walk's attributes in the Larder;
an open-file set-lock lands every open of the file, as a Proc's does; Stratum no longer
reports a failed lookup as a missing name; the rest are text. `posix_spawn`'s E2BIG past
64 names is stated in the manual and queued for the operator. Nothing here has booted.
