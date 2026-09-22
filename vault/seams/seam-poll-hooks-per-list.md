---
id: seam-poll-hooks-per-list
type: seam
title: "Nothing caps hooks on one poll_waiter_list, so the masked walks are attacker-scaled and the producer's crosses no preemption point"
status: open
surface: [sub-kernel-poll]
opened-by: fnd-b0poll-r7-f1
tracker: "v1.x"
created: 2026-09-22
updated: 2026-09-22
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

The list can hold 64 hooks per `poll` call, times `PROC_THREAD_MAX`, times the
number of Procs that can name the object. Nothing anywhere caps the product.

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
