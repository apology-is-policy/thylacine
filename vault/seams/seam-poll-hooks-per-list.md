---
id: seam-poll-hooks-per-list
type: seam
title: "Nothing caps hooks on one poll_waiter_list, so the masked walks are attacker-scaled and the producer's crosses no preemption point"
status: closed
surface: [sub-kernel-poll]
opened-by: fnd-b0poll-r7-f1
tracker: "v1.x"
created: 2026-09-22
updated: 2026-10-28
closed-by: chg-2026-10-28-poll-hook-walks
---
## Owed

A bound on how many hooks one `poll_waiter_list` can hold, and a bound on how
long a single masked walk of it may run.

`sched_preempt_point` bounds the NUMBER of masked spans a poller takes -- one
per pass -- and says nothing about the LENGTH of one. Two walks are unbounded
in the attacker's favour:

  - the poller's per-pass unregister walk, which at least crosses a point
    afterwards, and
  - the PRODUCER's `poll_waiter_list_wake` walk, which crosses no point at all,
    because it runs in the waker's context rather than the poller's.

The list can hold 64 hooks per `poll` call, times the Proc's thread budget
(`thread_budget`: 4096 by default since thread-model C6, where the fixed
`PROC_THREAD_MAX` was 256, and up to the hard maximum -- the pool's pages /
`THREAD_POOL_PAGES` -- for a Proc granted the raise), times the number of Procs
that can name the object. Nothing anywhere caps the product, and C6 raised its
default per-Proc factor sixteenfold.

## What closes it

Any one of three, and they are not exclusive:

  - the per-endpoint / event-keyed lists proposed at round-4 F8, which shrink
    every walk to the waiters that could actually be woken;
  - a per-walk wake cap with the remainder deferred to a following walk, which
    bounds the masked span directly at the cost of a second pass;
  - an I-32 axis capping hooks per list, which bounds the product at admission
    and is the only one of the three that makes the bound a stated resource
    rather than an emergent property.

## The risk while open

An I-27 latency claim rests on these walks: the SAK must be served promptly,
and a long masked walk delays it on that CPU. The exposure is a Proc that can
name a pollable object and spend threads registering on it -- which is the same
unprivileged shape as [[fnd-b0poll-r5-f1]], one level down. ARCHITECTURE.md
23.3 states the "one pass" bound with this qualification attached rather than
claiming a bound it does not have, and `docs/browser-status.md` carries it as
open B-0 residue.

## Closed (2026-10-28)

By the span, not the count ([[dec-2026-10-27-poll-walk-span]],
[[dec-2026-10-27-locked-walks-out]]): a wake walks at most 32 hooks per hold of
the list lock behind a stack bookmark, with IRQs restored and preemption possible
between holds; no walk runs under its owner's lock (table-lock walks begin under
it and finish after; 9P client-lock walks are deferred to the outermost release).
No hook cap and no new refusal. The total work stays O(pollers) -- each poller is
a pool-charged thread -- but no single masked or non-preemptible span grows with
it. See [[chg-2026-10-28-poll-hook-walks]].
