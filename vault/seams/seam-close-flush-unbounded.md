---
id: seam-close-flush-unbounded
type: seam
title: "The at-exit close-flush is unbounded and un-killable"
status: open
surface: [sub-kernel-death]
opened-by: fnd-68-r2-f3
tracker: "unfiled"
created: 2026-08-01
updated: 2026-10-07
---
## Owed

A bounded or abortable close-flush. `exit_close_active` suppresses BOTH
death legs for the closing thread, so a close-flush blocked on a wedged
server parks the dying Proc unreapably — and a further kill cannot break it
out, because the flag suppresses that too.

## What closes it

A deadline or cancellation on the write-behind flush and the close-time
Tclunk, so a wedged server yields a short flush rather than an indefinite
park. The kernel already has the shape elsewhere (the deadline-capable
transport recv), so this is a wiring question, not a design one.

## Risk while open

Bounded by its precondition: a wedged TRUSTED server, which is an already
system-degraded state. The exposure is not NEW — the equivalent strand
existed pre-#68 at reap time, where it hung the parent's `wait_pid` and
therefore the shell. #68 relocated it onto the already-dying Proc, which is
strictly better placed but no longer interruptible.

The honest framing recorded at the time: dropping the flag to restore
killability would reopen the silent data loss of [[fnd-68-r1-f1]]. This is a
trade, not an oversight.

## As of 2026-10-07

- The risk paragraph above is WRONG about its precondition: `SYS_ATTACH_9P`
  takes pipes from any process, so the server need not be trusted or wedged
  by accident -- any process can serve a mount and stop answering. A killed
  Proc then waits at exit wherever a close needs that server: a write-behind
  flush's reply, a free tag, room in a full request ring; the Loom SQPOLL
  kthread's reap clunk the same, and `loom_free`'s join with it.
- [[chg-2026-10-06-seam90-close]] made the elected reader unwind at any byte
  for a death, a stop or a caught note, so a killed Proc's reader no longer
  waits on its server; its at-exit close still does, under
  `exit_close_active`, where no death reaches it.
- Heritage researched (2026-10-06): Plan 9 has no timeout here either, and
  escapes by kill escalation -- a kill landing during `closefgrp` makes
  `sleep()` call `forceclosefgrp`, which hands the still-open channels to the
  `ccloseq` close-queue kprocs. Linux 9p and FUSE wait until the server hangs
  up or the connection is aborted. Options and the owner: OPEN-BUGS
  (2026-10-06 22:17Z).
- The operator voted on 2026-10-07: first the clunk side never waits at
  exit (the clunk goes to the closer, nothing lost); then a kill during the
  final close hands the rest of it, the write-behind run included, to the
  closer kthread, as Plan 9's `forceclosefgrp` does. No deadline: the "What
  closes it" paragraph above is superseded on that point. The tag-pool
  shortage (a full pool fails a sync op, and the write-behind flush then drops
  its data) is fixed first.
- [[chg-2026-10-07-tag-pool]] fixed that shortage. Since then the "free tag"
  wait at exit happens only when a session's op share (32767 ops) is full or a
  chunk of the tag table cannot be allocated; the write-behind flush's reply
  and room in a full request ring are unchanged.
