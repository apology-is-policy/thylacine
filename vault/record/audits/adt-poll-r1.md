---
id: adt-poll-r1
type: adt
title: "P5-poll — the formal round over the mechanism and both implementors"
date: 2026-05-20
scope: [sub-kernel-poll, sub-kernel-pipe]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: clean
counts: {p0: 0, p1: 2, p2: 2, p3: 7}
findings: [fnd-poll-r1-f1, fnd-poll-r1-f2, fnd-poll-r1-f3, fnd-poll-r1-f4]
round-of: chg-2026-05-20-p5-poll
created: 2026-08-01
---
## Scope

`kernel/poll.c`, both `.poll` implementors (devpipe + devsrv), and
every producer wake site — one prosecutor round at P5-poll-b.

## Verdict

Clean at close: two P1s (one fixed, one doc-fixed), two P2s fixed,
seven P3s dispositioned.

## The disposition that did not age

[[fnd-poll-r1-f3]] [P1] was closed by DOCUMENTING that
single-thread-per-Proc makes the handle-slot borrow safe. True when
written; voided silently by P6-pouch-threads; resurfaced at RW-2 as
a live UAF and closed structurally ([[fnd-rw2-2cf1]]). The pair is
this corpus's cleanest specimen of document-the-precondition versus
close-the-class — the class the batch-8 pins keep finding
(fnd-107-f1 is the same shape on the scheduler).
