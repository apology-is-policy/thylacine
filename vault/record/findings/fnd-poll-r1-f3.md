---
id: fnd-poll-r1-f3
type: fnd
round: adt-poll-r1
severity: P1
status: fixed
title: "The handle-slot borrow across the scan — doc-fixed on a precondition the lift later voided"
surface: [sub-kernel-poll]
threatens: [inv-i9]
fixed-by: chg-2026-06-10-rw2-poll-retain
regression: "none at the round; the structural close's discipline is pinned by the retain-order comments + the RW-2 close"
created: 2026-08-01
---
## Prosecution

`sys_poll_for_proc` resolved each fd to a `struct Handle *` — a
BORROW into the caller's table, valid only while nothing mutates the
table. The round's disposition: DOCUMENT that single-thread-per-Proc
makes the borrow safe (the poller's own thread is the only mutator).
True on the day it was written.

## The rest of the story

P6-pouch-threads made Procs multi-thread; the precondition evaporated
with no compiler, test, or grep to notice — a sibling could now close
a handle mid-poll. The class fired at HOLOTYPE RW-2 as a live UAF on
the object's embedded hook list ([[fnd-rw2-2cf1]]) and was closed
STRUCTURALLY: retain the obj ref for every registered waiter, release
after the sweep. (#844 independently rebuilt the handle-slot access
itself into a snapshot-with-held-ref API.)

Recorded as a linked pair with [[fnd-rw2-2cf1]] because the two
notes together are the corpus's cleanest specimen of
document-the-precondition vs close-the-class: the P5 disposition was
not WRONG — it was a time bomb armed by any future lift, with no
tripwire attached.
