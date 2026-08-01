---
id: chg-2026-07-05-33-sys-yield
type: chg
title: "#33 SYS_YIELD, and the #363 park-guard it uncovered"
date: 2026-07-05
arc: arc-go-build
commits: ["060fa97a", "8d0b9932", "5e5db6c5"]
touched: [sub-kernel-sched, sub-kernel-sched-smp]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
---
## What

`SYS_YIELD` = the EL0 voluntary entry to a transition the model already
had (`sched()` with prev RUNNING). The only new logic is a lock-free
fast path: skip `sched()` when the sole local occupant is the pinned
idle, because otherwise every spin-loop yield bounces through the idle —
two context switches for nothing, on a call the Go runtime issues **36.8
million times per `go build`**.

## The audit found something else ([[adt-33-r1]])

The yield mechanism itself audited sound. The finding was a
**pre-existing** bug in the idle park: [[fnd-33-r1-f1]] / #363.

`sched()` picks *before* it requeues prev. So a slice-expiry preempt of a
thread on an otherwise-empty local queue dispatches the pinned idle, and
the preempted thread lands in `run_tree[NORMAL]` right after the pick.
The dispatched idle does not restart its loop — it resumes **inside**
`sched_idle_park`, past its own `sched()`, headed straight for the
one-shot arm and the WFI, with no re-check. The CPU parked up to the 4 ms
backstop **over its own RUNNABLE thread**. No IPI exists for a local
self-requeue, and the one-shot had deasserted the periodic tick.

Up to ~4 ms lost per 6 ms slice for a solo compute-bound thread.

## Two things this taught, both worth keeping

**The telemetry had been read wrong.** The TI-4d multi-millisecond
starved-park records — including a 103 ms maximum — had been attributed
to peer backlog. They were this. The fix's witness is the collapse:
boot-wc tickless starved 9642 ms → 3404 ms per boot, −65%, at an
unchanged boot shape. The residual is the genuine peer-backlog signal the
counter was designed for.

**The benchmark was structurally blind.** The `scale` bench measures a
self-ratio, so a bug that slows every configuration equally is invisible
to it. And #33's own benignity model ("the idle immediately switches
back") rested on the same false premise the bug lived in — four doc sites
were corrected.

## Round 2 ([[adt-363-r2]])

Because the fix changed the park-commit logic, a second round ran on it:
clean, comment fixes only. Its one finding ([[fnd-33-r2-f1]]) is a
comment that misattributed the [[inv-i9]] guarantee to a *flag-gated*
IPI — inviting a future "optimization" that gates the cross-CPU placement
IPI on `idle_in_wfi`, whose bare-volatile store a peer can read
stale-FALSE. That would reintroduce up-to-backstop placement latency: the
exact class #363 closed. The comment now names both paths and forbids the
gate.

Recorded, no action: [[spec-sched-tickless]] **cannot express** #363 — it
has no self-requeue action. Acceptable for a latency-class bug bounded by
the backstop, and written down so the next change to the park-commit
logic does not read a green spec run as coverage.
