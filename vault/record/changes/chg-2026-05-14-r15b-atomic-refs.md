---
id: chg-2026-05-14-r15b-atomic-refs
type: chg
title: "r15-b: two refcounts made atomic before SMP could tear them"
date: 2026-05-14
arc: arc-phase5-ipc
commits: ["1025026f"]
touched: [sub-kernel-pipe]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

The r15-b audit round found the Spoor refcount (F233, P1) and the
pipe ring refcount (F234, P2, [[fnd-r15b-f234]]) as plain `--` RMWs
— the same race shape twice: two CPUs dropping concurrently lose an
update or both observe zero (double-free). Both became `__atomic`
ACQ_REL with pre-value ownership (`fetch_sub` pre == 1 owns the
free; pre <= 0 extincts).

F233's surface (the Spoor) belongs to the dev/spoor area; this chg
records the pipe half.
