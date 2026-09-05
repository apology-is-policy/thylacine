---
id: chg-2026-05-05-p2b-sched-dispatch
type: chg
title: "P2-Ba/Bb/Bc: dispatch, the Rendez wait/wake protocol, and tick preemption"
date: 2026-05-05
arc: arc-phase2-lifecycle
commits: ["a212038c", "4e108a89", "518d294e"]
touched: [sub-kernel-sched, sub-kernel-rendez]
established: []
closed: []
opened: [seam-eevdf-math, seam-runq-rbtree]
mirrors-checked: []
depth: skeletal
---
## What

The scheduler in three steps, all on one day.

**P2-Ba** — `sched()` (yield) and `ready(t)` (make runnable) over three
fixed-priority bands, each a doubly-linked list sorted by `vd_t`. Pick
the highest-priority non-empty band's head. `sched()` reads
`prev->state` and dispatches on it, so the caller decides whether it is
yielding or blocking.

**P2-Bb** — the Plan 9 wait/wake protocol: `struct Rendez`,
`sleep(r, cond, arg)`, `wakeup(r)`. Single waiter, atomicity from
`r->lock`: the condition is checked under the lock and the sleep
transition happens before it is released.

**P2-Bc** — scheduler-tick preemption (timer IRQ → slice decrement →
`need_resched` → `preempt_check_irq` → `sched()`) and the IRQ-mask
discipline that makes the wait/wake protocol robust against IRQ-context
wakers.

## Why this shape

The `vd_t` counter is a **simplification**, stated as such from the
first commit: advance a yielder's key past everything currently queued
and a band rotates FIFO. The weighted virtual-time math that would make
the name "EEVDF" true was scheduled for P2-Bc, on the reasoning that
"elapsed virtual time" is only meaningful once preemption exists.
Preemption landed; the math did not follow. [[seam-eevdf-math]].

The sorted linked list was chosen over the specified red-black tree for
the same class of reason — invisible at v1.0 thread counts, and easier
to walk lock-free from the diagnostics. [[seam-runq-rbtree]].

## Model

[[spec-scheduler]] was written alongside: `NoMissedWakeup` with the
split-protocol counterexample at depth 4 is the wait/wake half.
