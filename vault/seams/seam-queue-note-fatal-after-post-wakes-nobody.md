---
id: seam-queue-note-fatal-after-post-wakes-nobody
type: seam
title: "A process-directed caught note made fatal after its post wakes nobody"
status: open
surface: [sub-kernel-notes]
opened-by: fnd-thread-c4-r2-f1
tracker: "OPEN-BUGS 2026-11-02T11:37Z; owned by item 11c"
created: 2026-11-02
updated: 2026-11-02
---
## Owed

A note posted to the process queue under a handler arms no terminate latch. If
`SYS_NOTIFY(0)`, `rt_sigaction` SIG_DFL or `SA_RESETHAND` then makes the disposition
fatal, the note stays unarmed, and a peer in a wait sleeps on until the wait ends or
another post re-arms. The unregistering thread's own tail skips it if that thread
masks the family. A delay, not a loss. `proc_note_direct_rouse` covers the per-thread
words only. Two tests pin "unregister does not retro-arm" as a contract (LS-5c).

## What closes it

Item 11c decides it: the defect bites a native wait that rode the note out at post,
which 11c changes. What remains after that is a hold wait, which is Linux-equivalent.
11c either rouses from the three stores as C4's words do, or records why not and
changes the two tests' reading.
