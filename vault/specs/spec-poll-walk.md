---
id: spec-poll-walk
type: spec
title: "poll_walk.tla"
models: [sub-kernel-poll]
pins: [inv-i9]
cfgs:
  - "poll_walk.cfg -- clean: K = 1, three hooks, two walkers; Covered + NoStray + MarkIffParked (266 distinct states)"
  - "poll_walk_k2.cfg -- clean: K = 2 (170)"
  - "poll_walk_liveness.cfg -- WalkEnds under weak fairness of the walkers only; the hooks churn with none (266)"
  - "poll_walk_buggy_tail_insert.cfg -- register appends at the TAIL, the Linux 2017 bookmark shape removed in v6.7 for livelock (WalkEnds counterexample)"
  - "poll_walk_buggy_tail_insert_stray.cfg -- the same insert reaches a hook registered after the walk began (NoStray counterexample)"
  - "poll_walk_buggy_resume_skips.cfg -- a resumed walk skips the hook after its bookmark (Covered counterexample)"
  - "poll_walk_buggy_mark_ends.cfg -- another walk's bookmark taken for the end of the list (Covered counterexample)"
gate: "any change to poll_walk's batch, its bookmark park/resume, register's insert position, or unregister's unlink -- specs/check-poll.sh"
created: 2026-10-28
updated: 2026-10-28
---
## Abstraction

One `poll_waiter_list`, hooks that register and leave with no fairness at all,
and two producers walking it in batches of K behind stack bookmarks
([[dec-2026-10-27-poll-walk-span]]). Each batch is one hold of the list lock;
between holds the walk's bookmark sits right behind the last hook it reached.

## What it pins

- **Covered** -- an ended walk reached every hook it OWED: the hooks listed
  when it began and still listed. A hook that left was owed nothing; one that
  registered later sampled the mutation itself (register-then-observe).
- **NoStray** -- a walk reaches nothing else, and each hook at most once.
- **MarkIffParked** -- a walker's bookmark is listed exactly while it is parked.
- **WalkEnds** -- every walk ends, however the hooks churn: register pushes at
  the HEAD and the walk goes head to tail, so a walk never meets a hook newer
  than itself. The tail insert is the bug that killed Linux's bookmark
  (2554db916586, removed by b0b598ee08f9).

## What it cannot see

The per-hook wakeup and the rendez (poll.tla's Reach); lifetimes of the list's
owner (poll_owed.tla and poll_defer.tla carry the pin).

## Binding

`specs/SPEC-TO-CODE.md::poll_walk.tla`: Batch <-> `poll_walk`'s one hold
(`POLL_WAKE_BATCH`, the park behind `last`, the resume from `mark->next` with
`poll_list_unlink`); Register <-> `poll_waiter_list_register` (head push);
Unregister <-> the O(1) doubly-linked unlink with both neighbour checks.
