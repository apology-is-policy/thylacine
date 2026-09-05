---
id: chg-2026-05-23-p6-threads-b
type: chg
title: "P6 sub-chunk 9b: pthreads over SYS_THREAD_SPAWN + torpor"
date: 2026-05-23
arc: arc-phase6-pouch
commits: ["551be97f"]
touched:
  - sub-pouch-thread
established: []
closed: ["fnd-threads9b-r1-f1", "fnd-threads9b-r1-f2", "fnd-threads9b-r1-f5"]
opened: ["seam-pouch-guard-pages", "seam-pouch-process-shared"]
mirrors-checked: []
depth: skeletal
created: 2026-08-01
---
Eight files: the four Thylacine extension numbers, the inline
`__wake`/`__futexwait` retarget, `__futex4_cp` onto torpor's relative
microseconds, `pthread_create`'s spawn + clear-child-tid registration,
the exit paths, and `__unmapself`'s two-syscall asm.

[[adt-threads9b-r1]] closed 2 P1: the >1-hour timeout that became a
100%-CPU spin ([[fnd-threads9b-r1-f1]]) and the silently-absent stack
guard pages ([[fnd-threads9b-r1-f2]], documented not fixed -- it needs a
kernel VMA-permission syscall). Its F5 -- the build's seam-check list not
extended for the round's four new numbers -- recurs verbatim at the
signals round.
