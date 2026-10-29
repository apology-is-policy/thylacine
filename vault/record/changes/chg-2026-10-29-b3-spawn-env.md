---
id: chg-2026-10-29-b3-spawn-env
type: chg
title: "b3 backports the spawn environment tail (env-a + env-c); the child is born with no environment"
date: 2026-10-29
arc: arc-boosty
commits: ["3db0e350d"]
touched:
  - sub-kernel-syscall-abi
  - sub-kernel-syscall-dispatch
  - sub-kernel-content
  - sub-kernel-exec
  - sub-kernel-proc
  - sub-kernel-birth-hold
  - sub-kernel-caps
  - sub-libthyla-rs
  - sub-stratum-boot
  - sub-coreutils-filters
  - sub-pouch-process
  - sub-tyrquake
  - sub-quarry
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-29
---
Operator vote 33 gave b3 the spawn environment tail ([[chg-2026-10-28-spawn-env]])
by cherry-picking corona's env-a and env-c, not by merging corona's branch. The
kernel was ported by hand to a tree without the thread model: bit 1 is refused,
and the tail sits at +104, or +120 after a cwd tail. Two Opus rounds found the
child held its spawner's whole environment until its thunk ran (fixed with the
kernel-internal `RFCENVG`), the tail admitted `.` and `..` (fixed in
`name_valid`), and pouch's `environ` could carry a variable the tail cannot (a
name holding `=` or a value holding a NUL; 0025 now skips both, held by
`tools/test-pouch-env-init.py`). [[dec-2026-10-29-spawn-env-r2]] restates the
decision with these.
