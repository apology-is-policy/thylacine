---
id: seam-exiting-tails-never-sleep
type: seam
title: "EXITING tails must never SLEEP — a property held by accident, not enforcement"
status: open
surface: [sub-kernel-death]
opened-by: chg-2026-07-14-68-last-thread-out-close
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
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

## Risk while open

None today. The failure mode when it breaks is an extinction on the exit
path — loud, but on the death lineage, which is where loud failures have
historically been hardest to attribute.
