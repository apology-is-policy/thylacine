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

Operator vote 25, 2026-11-03: the disposition leaves the mask. Pouch's SIGPIPE default is a row
that ignores and a real handler a caught row, each per Proc ([[dec-2026-11-03-sigpipe-row]],
patch 0071). This closes at that chunk's landing, once `pouch-hello-signals pipe-thread` has
run.
