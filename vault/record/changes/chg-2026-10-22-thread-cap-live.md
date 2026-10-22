---
id: chg-2026-10-22-thread-cap-live
type: chg
title: "The thread cap charges live threads (thread-model C1, vote 7)"
date: 2026-10-22
arc: arc-phase6-pouch
commits: ["*(pending)*"]
touched:
  - inv-i32
  - sub-kernel-proc
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-10-22
---
A follow-on inside thread-model C1 ([[chg-2026-10-22-thread-model-c1]]), raised
by corona and confirmed from the code. Both exit paths wake a joiner (the
clear-child-tid wake) right after the EXITING commit and before the final
switch-out whose resume path frees the thread ([[inv-i48]]), so a cap on
`thread_count` refused a Proc at `PROC_THREAD_MAX` that had just joined a
thread -- at both spawn arms, the SQPOLL charge and the private executor's
charge. Operator vote 7: the cap counts live threads. `proc_thread_room_locked`
reads `live_threads + loom_sqpoll_count`; the list may exceed the cap only by
the exited threads not yet freed, one per CPU ([[inv-i32]]). Spec first:
[[spec-thread-reap]]'s `ExitedNotCharged` and `ListBound`, with a buggy cfg
that restores the old count. Rejected: a create that waits out the lag, and
leaving the transient refusal. Witness: `proc.cap_counts_live_threads`.
