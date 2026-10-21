---
id: seam-exiting-tails-never-sleep
type: seam
title: "EXITING tails must never SLEEP — a property held by accident, not enforcement"
status: open
surface: [sub-kernel-death]
opened-by: chg-2026-07-14-68-last-thread-out-close
tracker: "unfiled"
created: 2026-08-01
updated: 2026-10-20
---
## Owed

The #68 close window's soundness rests on a stated property: an EXITING
peer's residual execution never touches the handle table and never SLEEPS.
The second half is what is owed, because it is currently true by
COINCIDENCE rather than by construction.

An EXITING thread's remaining work is the clear-child-tid handoff plus
`sched()`. The handoff does `uaccess_store_u32` into a user VA — which
COULD demand-page. Sleeping while EXITING trips `sched()`'s "current is not
RUNNING" assertion, i.e. extincts.

It does not happen today because every writable VMA is eager or lazy-anon
(the lazy arm resolves fully under `vma_lock`, without blocking) and FILE
Burrows are never writable — the REVENANT dispatch gate keeps `PF_W`
segments eager.

## What closes it

Either an explicit non-sleeping guarantee on the exit-tail uaccess path, or
an enforcement that catches the violation instead of relying on the mapping
taxonomy staying as it is.

The v1.x anon-COW / pageout work is the trigger: the moment a writable
mapping can fault into a BLOCKING arm, this property must be
re-established deliberately, before that work lands.

## The preempt half (enforced 2026-10-20)

The same property has a second half this seam did not record: a tail must not
be switched out before its own `sched()`. On a kthread-shaped path
(`userland_enter`'s die-check, an exec failure's exit) the tail runs with
interrupts on and neither the preempt count nor the in-syscall marker set, so an
interrupt return could switch it out, and an EXITING thread is never requeued:
a last thread out lost its srv, cap and weft notifies. `preempt_check_irq` now
returns without switching for a thread that is not RUNNING ([[sub-kernel-sched]];
regression `proc.exit_tail_runs_to_its_end`). The sleep half stays owed.

A new dependent: exec waits for every peer to leave its CPU before it swaps the
address space ([[sub-kernel-proc]]). A tail that slept would leave its CPU as
SLEEPING, not EXITING, so exec's reap would not free it and the swap's live-peer
re-check would extinct.

## Risk while open

None today. The failure mode when it breaks is an extinction on the exit
path — loud, but on the death lineage, which is where loud failures have
historically been hardest to attribute.
