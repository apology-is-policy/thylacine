---
id: seam-close-flush-unbounded
type: seam
title: "The at-exit close-flush is unbounded and un-killable"
status: open
surface: [sub-kernel-death]
opened-by: fnd-68-r2-f3
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
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
