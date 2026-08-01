---
id: chg-2026-07-28-v4b4-readlink
type: chg
title: "VIVARIUM V-4b-4: readlink on a system with no symlinks"
date: 2026-07-28
arc: arc-vivarium
commits: ["c6ca9f09"]
touched:
  - sub-pouch-fs
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
The seam parked `readlinkat` at the sentinel, which is the WRONG answer
rather than an absent one: on a symlink-free system the result is known
for every path, and POSIX has the words -- an existing non-link is
`EINVAL`.

The distinction turned out to be far larger than the `/proc/self/exe` gap
that surfaced it. musl's `realpath()` is a pure userspace resolver that
calls `readlink` on each prefix and reads the errno as a fork in the
road: `EINVAL` means keep walking, any other errno is fatal. Under
`ENOSYS`, `realpath()` failed on its FIRST component -- so it was broken
for every path on the system, for every ported program, and the truthful
`EINVAL` repairs it whole with no realpath patch at all.

The `/proc` arm translates shape, not contents: four paths Linux presents
as symlinks are regular files here whose contents ARE the target, so
readlink is an open+read for exactly those four -- a closed whitelist
whose MISS falls through to `EINVAL`, which is true of every file
served.
