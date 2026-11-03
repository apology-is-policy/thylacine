---
id: spec-poll-owed
type: spec
title: "poll_owed.tla"
models: [sub-kernel-poll, sub-kernel-proc]
pins: [inv-i9, inv-i8]
cfgs:
  - "poll_owed.cfg -- clean: two holders, two owners, one hook each; every invariant (2046 distinct states)"
  - "poll_owed_h2.cfg -- clean: one owner, two hooks (414)"
  - "poll_owed_liveness.cfg -- OwedReached + DrainEnds (2046)"
  - "poll_owed_buggy_skip_drain.cfg -- a release drops the lock without draining (ChainDrained counterexample)"
  - "poll_owed_buggy_skip_drain_live.cfg -- the same, judged on liveness (OwedReached counterexample)"
  - "poll_owed_buggy_no_cancel.cfg -- the reap leaves the Proc's record chained (NoUAF counterexample)"
  - "poll_owed_buggy_free_no_wait.cfg -- proc_free does not wait out the bookmarks (NoUAF counterexample)"
  - "poll_owed_buggy_begin_after_unlock.cfg -- the bookmark listed after the drop (NoUAF counterexample)"
  - "poll_owed_buggy_owe_joins_walk.cfg -- an event on a list whose walk is in flight chains nothing (OwedCovered counterexample)"
  - "poll_owed_buggy_drain_all.cfg -- a release drains every holder's records (DrainEnds counterexample)"
gate: "any change to proc_table_lock_acquire/_release, proc_wake_or_owe, poll_owe / poll_owed_cancel / poll_owed_begin, the reap's cancel, or proc_free's bookmark wait -- specs/check-poll.sh"
created: 2026-10-28
updated: 2026-10-28
---
## Abstraction

Walks owed under `g_proc_table_lock` ([[dec-2026-10-27-locked-walks-out]]).
That lock is IRQSAVE, so a walk under it stays masked however it is batched,
and it is also what keeps a Proc alive (Procs have no refcount). An event under
the lock OWES a walk: it chains the list's record on the holder's own chain,
once. Every release drains the holder's chain: still under the lock it pops a
record and lists a stack bookmark at the head (the walk BEGINS), drops the
lock, walks, and retakes it for the next. The bookmark is the PIN: the reap
cancels the Proc's records under the lock, and `proc_free` waits until no
bookmark is on its lists.

## What it pins

- **NoUAF** -- no walk touches a freed Proc's list or record.
- **ChainDrained** -- a holder out of the lock owes nothing.
- **OwedCovered** + **OwedIsListed** + **MarkIffWalking** -- every owed hook is
  still to be reached: its record chained (a walk that will begin at the head)
  or a walk in flight with its bookmark ahead of it.
- **OwedReached** + **DrainEnds** -- liveness: every owed hook is reached or
  leaves; every drain ends (a holder drains only its OWN chain).

## What it cannot see

That the walker can be preempted between batches and the free-side waiter can
be a never-preempted syscall body: the model gives the drainer weak fairness,
which a single CPU does not give a runnable kthread against such a spinner.
Audit r1 F1 ([[adt-pollwalk-r1]]) found exactly that; the wait now yields the CPU,
and the regression is runtime, not here.

## Binding

`specs/SPEC-TO-CODE.md::poll_owed.tla`: Acquire / Release <-> the wrappers (the
ONLY path to the lock; a release by a thread that did not take it through the
wrapper extincts); Event <-> `proc_wake_or_owe` at the six child_waiters sites,
`notes_post` and the console episode; Unlink <-> `proc_unlink_child`'s cancel;
Free <-> `proc_free`'s extinction on a chained record + its bookmark wait.
