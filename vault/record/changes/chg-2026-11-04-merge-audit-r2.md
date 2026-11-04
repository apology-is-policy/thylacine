---
id: chg-2026-11-04-merge-audit-r2
type: chg
title: "Merge audit r2's close on the join"
date: 2026-11-04
arc: arc-halcyon-interaction
commits: []
touched:
  - sub-kernel-sched
  - sub-kernel-thread
  - sub-kernel-proc
  - sub-substrate-build
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-04
---
Round 2 of the second main integration: 1 P0, 1 P1, 3 P2, 4 P3, all closed. The P1 is the one that changes a mechanism. Vote 64 ([[dec-2026-11-04-compositor-band]]) accepted starvation "on the CPUs it occupies, at most two", but two described today's binary and nothing held a compromised compositor to it, since it can make threads freely and is exempt from the thread budget. `thread_link_into_proc_locked` now rations the band: a thread takes it only when joining with fewer than `SEAT_CLIENT_BAND_THREADS` live, under the lock it joins under. That enforces the bound the vote accepted rather than widening it. The P0 and one P2 were both gate faults that no build had yet met: an optional `--logs` read as the positional convention, and a grep for a word five drivers never print. Aux's pinned-wake follow-up hunks from 9059174e3 (the wake IPI for a pinned thread, the ready_on guard, the runner's attribution, the waker's sched(), and its regression test) came with this close, in aux's text.
