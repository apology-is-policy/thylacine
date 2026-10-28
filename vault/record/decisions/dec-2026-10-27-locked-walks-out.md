---
id: dec-2026-10-27-locked-walks-out
type: dec
title: "Every hook walk that ran under a lock moves out of it"
date: 2026-10-27
status: standing
decided-by: user-vote
affects: [sub-kernel-poll, sub-kernel-proc, sub-kernel-cons, sub-kernel-notes, sub-kernel-ninep-client, sub-kernel-srvconn, sub-kernel-pipe, sub-kernel-loom]
created: 2026-10-28
---
## Fork

A batched walk still runs masked under `g_proc_table_lock` (IRQSAVE) and
non-preemptible under a 9P client's `c->lock`. Moving those walks out needs a
lifetime story, because the lock is what keeps the owner alive.

## Decision

Operator vote 13 (2026-10-27 21:14Z): all of it. Table-lock walks begin under the
lock with a bookmark at the head (the pin) and finish after the unlock, at every
release, through a wrapper; `proc_free` waits until no bookmark is on its lists;
the console episode's walks run after the table lock drops; the 9P, pipe, srvconn
and loom walks move after `c->lock` drops, using the refs they hold. Spec first.

Implementation refinements, recorded in the commits, not re-asked: an event OWES
a per-list record on the holder's per-thread chain (a pgrp walk posts to
unboundedly many Procs in one hold, so one bookmark per event is unbounded
storage) -- [[spec-poll-owed]]; past `c->lock`, references only where the owner is
refcounted and its free is safe in the drainer's context (srvconn, pipe ring),
WAIT owners elsewhere (the Loom: an async op does not reference its Loom, and a
reference could make the SQPOLL kthread run `loom_free` on itself) --
[[spec-poll-defer]].
