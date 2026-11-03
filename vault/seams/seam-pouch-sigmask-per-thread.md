---
id: seam-pouch-sigmask-per-thread
type: seam
title: "`sigaction(SIGPIPE)` updates only the calling thread's kernel mask"
status: open
surface: [sub-pouch-signal]
opened-by: chg-2026-05-24-p6-signals-b
tracker: "signals-13b F-SELF-1"
created: 2026-08-01
updated: 2026-11-03
---
## Owed

The kernel's `note_mask` is per-Thread -- correct for `pthread_sigmask`,
wrong for `sigaction`. A multi-thread Proc that calls
`sigaction(SIGPIPE, &handler, NULL)` on thread A clears `NOTE_BIT_PIPE`
for A only; every other thread still has SIGPIPE masked and will not run
the handler. POSIX requires the disposition change to apply
process-wide.

A new thread inherits its creator's mask since 0065, so which threads see
the note depends on which thread called `sigaction` before which was spawned.

## The lift

Either a Proc-wide flag on `SYS_NOTE_MASK`, or `sigaction` iterating the
Proc's threads. The kernel-side flag is the cleaner shape and also fixes
inheritance if the spawn path copies the parent's mask.
