---
id: dec-2026-10-27-poll-walk-span
type: dec
title: "Bound a hook walk's span, not the number of hooks"
date: 2026-10-27
status: standing
decided-by: user-vote
affects: [sub-kernel-poll]
created: 2026-10-28
---
## Fork

Nothing capped the hooks on one `poll_waiter_list` ([[seam-poll-hooks-per-list]]),
and a producer's wake walked the whole list under one irqsave hold. Options: bound
the SPAN (batched walk behind a bookmark, O(1) unregister, one hook per poll call
per list; no new refusal); a per-list hook cap (an I-32 axis, refusal past N -- a
hostile client then refuses legitimate pollers); a wake hand-off (a stalled waker
stalls the rest); per-event lists (fewer irrelevant hooks, not a bound).

## Decision

Operator vote 12 (2026-10-27 20:57Z): bound the span. Walks reach at most
`POLL_WAKE_BATCH` (32) hooks per hold of the list lock and park a stack bookmark
between holds; unregister is O(1) on a doubly linked list; a poll call keeps one
hook per list. Nothing new is refused. Spec first ([[spec-poll-walk]]). Linux's
2017 bookmark was removed for livelock because register appended at the tail;
here register pushes at the head, so a walk never meets a newer hook.
