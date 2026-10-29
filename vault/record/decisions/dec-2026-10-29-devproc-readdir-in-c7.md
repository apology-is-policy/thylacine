---
id: dec-2026-10-29-devproc-readdir-in-c7
type: dec
title: "devproc readdir lands with the per-thread files"
date: 2026-10-29
status: standing
decided-by: user-vote
affects: [sub-kernel-devproc]
created: 2026-10-29
---
## Fork

`ls /proc` answered EOPNOTSUPP (seam-932's devproc half). The voted per-thread
layout is operator-visible only if it can be listed, so the listing either lands
in C7 or the layout ships unbrowsable.

## Decision

Operator vote V2 (2026-10-29 ~08:37Z): readdir stays in C7. The root, pid,
thread and tid directories list through a cursor with no snapshot, batches of
16, cookie = id + 1; ids are never reused but by exec's pid move
([[sub-kernel-devproc]]). seam-932 keeps its devsrv half.
