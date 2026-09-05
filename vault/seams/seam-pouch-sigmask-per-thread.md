---
id: seam-pouch-sigmask-per-thread
type: seam
title: "`sigaction(SIGPIPE)` updates only the calling thread's kernel mask"
status: open
surface: [sub-pouch-signal]
opened-by: chg-2026-05-24-p6-signals-b
tracker: "signals-13b F-SELF-1"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`__pouch_note_mask_shadow` is `__thread` TLS, and the kernel's
`note_mask` is per-Thread -- correct for `pthread_sigmask`, wrong for
`sigaction`. A multi-thread Proc that calls
`sigaction(SIGPIPE, &handler, NULL)` on thread A clears `NOTE_BIT_PIPE`
for A only; every other thread still has SIGPIPE masked and will not run
the handler. POSIX requires the disposition change to apply
process-wide.

Compounded by the absence of mask inheritance at `pthread_create` (a
child starts at mask 0), so which threads see which notes depends on
spawn order.

## The lift

Either a Proc-wide flag on `SYS_NOTE_MASK`, or `sigaction` iterating the
Proc's threads. The kernel-side flag is the cleaner shape and also fixes
inheritance if the spawn path copies the parent's mask.
