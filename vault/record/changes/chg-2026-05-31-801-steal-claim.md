---
id: chg-2026-05-31-801-steal-claim
type: chg
title: "#801-F1: claim the stolen thread under the victim's own lock"
date: 2026-05-31
arc: arc-holotype-rw
commits: ["6db3a561"]
touched: [sub-kernel-sched-smp]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
## What

`try_steal` now sets `stolen->on_cpu = true` **under the peer's lock,
before releasing it** — rather than leaving the claim to the picker
after the lock is dropped.

## The window

Between unlinking a victim from a peer's tree and the picker claiming it,
the thread sat out-of-tree, RUNNABLE, and `on_cpu == false` in an
**unlocked limbo**. A concurrent `thread_free` could observe it free-able
and reclaim its context and kernel stack mid-steal — a use-after-free on
a thread another CPU was about to load.

Claiming under the peer's lock closes it by making the two paths meet on
one lock: a racing `thread_free`, whose `sched_remove_if_runnable` walk
takes the same peer lock, either unlinked the thread **first** (so the
steal never finds it) or observes `on_cpu == true` after its walk and
waits the steal out on its own `on_cpu` spin.

## Reachability, stated honestly

Production-unreachable at the time: only tests free a RUNNABLE, in-tree
thread. It was closed anyway, for any future caller — which is the same
posture [[fnd-866-r1-f2]] took a week later on the steal band-walk, and
the same reasoning [[seam-sparse-mpidr]] records for the MPIDR check. On
this surface a window that is unreachable *today* is a defect, because
the thing that makes it reachable is a new caller, not a new bug.

The store is RELAXED, matching the picker's; the peer-lock release is the
inter-CPU publish edge.
