---
id: chg-2026-11-02-b3h-audit-r3-fixes
type: chg
title: "B-3h audit round 3's fixes"
date: 2026-11-02
arc: arc-boosty
commits: ["8afd8eb87", "3549ab3c6"]
touched:
  - sub-kernel-filelock
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-session
  - sub-kernel-ninep-client
  - sub-kernel-ninep-attach
  - sub-kernel-larder
  - sub-kernel-handle
  - sub-kernel-spoor
  - sub-kernel-dev
  - sub-kernel-stalk
  - sub-kernel-thread
  - sub-kernel-loom
  - sub-kernel-syscall-dispatch
  - sub-pouch-fs
  - seam-close-flush-unbounded
established: []
closed: []
opened: []
mirrors-checked: [docs/ARCHITECTURE.md, docs/FID-LIFECYCLE-DESIGN.md, docs/LARDER-DESIGN.md, docs/POUNCE-DESIGN.md, docs/ERRORS.md, docs/AUDIT-TRIGGERS.md, docs/manual/21-environment.md, docs/manual/22-file-locks.md]
depth: skeletal
created: 2026-11-02
---
Audit round 3 on B-3h returned 1 P0, 2 P1, 3 P2 and 15 P3 ([[dec-2026-10-29-b3h-file-locks]]).
The P0 was a Stratum commit that had never compiled, fixed on the private clone. The
two P1s and one P2 had one cause: round 2 hung the release and the flush on a Spoor's
last reference, and the image cache can hold a descriptor's own Spoor until 128 other
images churn. A read-only map kept an open-file lock past its Proc's exit, left staged
bytes unlanded after `close` returned 0, and made a stranger's `exec` wait on the
owner's server.

A descriptor's last close is now separate from a Spoor's last reference, as Linux's
`f_op->flush` is from `f_op->release`. `Spoor.fd_refs` counts handle slots; the last
slot runs `Dev.fd_close` on the closing thread, which may wait; `Dev.close` waits on no
thread, so `closes_never_wait` is gone. A take racing that close answers EBADF through a
sequentially consistent pair. This is an implementation fix inside ARCH 9.6.11's
sentence, not a vote. Its visible cost: a read-only map no longer keeps a `flock` where
Linux's `fput` would.

Also: `larder_forget_all` for a mutating op whose owner was killed, with its residual
stated in LARDER-DESIGN section 12; a walk of several names failing with anything but
ENOENT re-walks name by name, so the X gates and mount split run in order;
`lseek(fd, 0, SEEK_CUR)` no longer stores the cursor; `SEEK_END` on a Dev with neither a
stat nor locks answers ENOLCK. Nothing here has booted yet: the lease owes every new
leg and mutant. A fourth round is owed because the fix restructured a load-bearing
mechanism.
