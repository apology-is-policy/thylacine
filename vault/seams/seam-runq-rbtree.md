---
id: seam-runq-rbtree
type: seam
title: "The run tree is a sorted linked list, so insert is O(N)"
status: open
surface: [sub-kernel-sched]
opened-by: chg-2026-05-05-p2b-sched-dispatch
tracker: "ARCH 8.4 design intent; Phase 7"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

The red-black tree ARCH §8.4 specifies. Today each band is a doubly-linked
list kept ascending by `vd_t`: insert walks it (O(N)), remove is O(1),
pick is O(1) per band.

## Why it was deferred

With realistic thread counts — tens, not thousands — the tree's O(log N)
advantage is invisible, and the list is far easier to reason about in a
structure that is walked lock-free by three diagnostics
(`sched_runnable_count`, `sched_has_runnable_work`,
`sched_dump_runnable`). The API does not change when it is replaced.

## Cost of leaving it

Bounded and known. The insert cost is paid on every `ready()` and every
yield-requeue, so it scales with runnable threads per CPU, not with total
threads. It becomes interesting only if a workload pushes a single CPU's
runnable set into the hundreds — which no v1.0 workload does, the on-device
Go build included.

Note when it *is* replaced: `in_run_tree`'s three-way test (`next ||
prev || head == t`) exists because a sole list element has both links
NULL. Whatever replaces it needs an equally cheap membership test, since
`ready_on` extincts on an already-queued thread and `sched_remove_if_runnable`
walks every CPU looking for one.
