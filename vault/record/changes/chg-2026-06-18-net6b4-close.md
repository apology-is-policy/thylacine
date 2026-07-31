---
id: chg-2026-06-18-net6b4-close
type: chg
title: "net-6b-4: the dev9p.poll focused audit close (two rounds, converged)"
date: 2026-06-18
arc: arc-net
commits: ["caa4db3d", "297666d2"]
touched: [sub-kernel-ninep-dev9p-poll]
established: []
closed: [fnd-net6b-r1-f1, fnd-net6b-r1-f2, fnd-net6b-r1-f5, fnd-net6b-r2-f2]
opened: [seam-221-idle-pump-wake, seam-223-pump-tail-starvation]
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
[[adt-net6b-r1]] (0/1/0/4, DIRTY -- the P1 fix restructured the pump loop)
-> [[adt-net6b-r2]] on the fixes (0/0/0/2, CONVERGED CLEAN). F4 (the pouch
ready-fd slot-reuse ABA, task #222) backfills with the pouch sweep; the
netd listener-poll scope-out (task #220) with the netd sweep. The owed
two-QTPOLL-client fairness regression for F1 is recorded in
[[fnd-net6b-r1-f1]] (no in-tree config drives two clients).
