---
id: chg-2026-10-29-b3h2-file-locks
type: chg
title: "B-3h2: SYS_FILE_LOCK 137 with its two owners, the Proc owner table, and dev9p's lock path"
date: 2026-10-29
arc: arc-boosty
commits: ["f30f2a9eb", "d1853e821", "c95ef3916", "b693e2729", "80e99cae5", "*(pending)*"]
touched:
  - sub-kernel-filelock
  - sub-kernel-handle
  - sub-kernel-ninep-dev9p
  - sub-kernel-ninep-attach
  - sub-kernel-ninep-client
  - sub-kernel-ninep-session
  - sub-kernel-ninep-wire
  - sub-kernel-syscall-abi
  - sub-kernel-vivarium
  - sub-kernel-dev
established: []
closed: []
opened: []
mirrors-checked: [kernel/include/thylacine/errno.h, docs/ERRORS.md, docs/ARCHITECTURE.md, docs/FID-LIFECYCLE-DESIGN.md]
depth: skeletal
created: 2026-10-29
---
The kernel half of [[dec-2026-10-29-b3h-file-locks]] (operator votes 34-35)
landed on branch b3h for fid-backed Spoors: the 9P Tlock/Tgetlock client,
`SYS_FILE_LOCK` naming its owner, the Proc owner table in the handle table,
dev9p's lock slots with lock-to-unlock consistency, and the closer's job-alone
entry. Three defects were found in self-review before any build, all from one
Stratum fact: an unlinked file's fids die at once, and its inode number goes
to the next create.
- A stale entry blocked the new file: an entry whose fid answers ESTALE is now
  retired, and the call retries once.
- A dead descriptor acted on the new file: identity now includes the inode
  generation, learned once per Spoor.
- A killed holder of a shared open file released before its staged writes
  landed: the flush now goes to the closer as a job alone, ahead of the
  owner's clunk.

The pre-lease object check also found a link break: a test's aggregate zero
became a `memset` once the dev9p priv grew. Fidless cached opens (B-3h3),
Pouch `fcntl`/`flock` (B-3h4), and the one Opus audit over all of B-3h
remain.
