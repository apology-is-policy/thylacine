---
id: chg-2026-11-09-park-readbacks-r2f1
type: chg
title: "A released chain is owed, not counted, until its context is vindicated (vote 81 audit round 2 F1)"
date: 2026-11-09
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-lictor
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-09
---
Follows [[chg-2026-11-09-park-readbacks-f1]]. Round 2's F1: a released parked chain (ok = false, not abandoned) took the seam's counting arm, so the dense `fence_signaled` passed the abandoned fence ahead of it, and the winsys's wait (signaled >= seq, which never reads the poison) returned success for the abandoned chain and for the released readback, whose bytes never landed. The dead-engine release had the same flaw. `FenceTag` gains `released` (both release paths set it; the wire record carries it). `warp_service_fences` treats a released tag as no completion: it poisons the context, so the fence stream is over and a waiter reads EOF, and owes the count in `WarpCtx::released_owed`, per timeline. The vindication that un-poisons the context pays it into `fence_signaled` and `timeline_signaled`. The C-6 gate's ABANDON leg now also requires `fence-signaled` unmoved while the context is poisoned and exactly four higher after the hold's replay vindicates the queue.
