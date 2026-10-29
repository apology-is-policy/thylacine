---
id: chg-2026-10-02-ordered-progress
type: chg
title: "Drive retained ordered-stream work before sleeping"
date: 2026-10-02
arc: arc-halcyon-interaction
commits: []
touched: [sub-libtapestry, sub-halcyond]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The session's ordered channel supplies a runnable hint for retained records and
unsubmitted I/O. Its existing poll timeout now respects that hint rather than
waiting for an unrelated HSC State wakeup to rearm a read. Pending I/O and early
decisions waiting for Rwrite do not spin. HSC still runs first; no authority,
thread, timer or public clipboard endpoint is added.

23 libtapestry tests, 17 actual-source cases, the peer predicate, ten intended
mutations and guest compilation pass. Native results are in the interaction
status. Single-agent self-review, October 2 gate waiver and draft preservation
remain in force.
