---
id: adt-thread-usr-r1
type: adt
title: "The usr1 and usr2 notes round 1, before the first boot"
date: 2026-11-02
scope: [sub-kernel-notes, sub-kernel-proc, sub-kernel-thread, sub-kernel-vivarium, sub-pouch-signal, sub-libthyla-rs]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 2}
findings: [fnd-thread-usr-r1-f1, fnd-thread-usr-r1-f2, fnd-thread-usr-r1-f3]
round-of: chg-2026-11-02-usr-notes
created: 2026-11-02
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on 59503aa78 in a frozen worktree, while the code had only been compiled. Not dirty: a test expectation, a guard, docs, and no kernel behaviour changed, so no second round is owed. It enumerated every reader and writer of the family words from the tree, and found no narrow cast left. What it could not see the leases found: a stale exact pin in test_vivarium, and three sabotages that stayed green (see the change note). Its figure of 472 for sizeof(struct Proc) is wrong; proc.h asserts 504. Read least deeply: the arch EL0 tail, proc_stop_sleeper_park, poll.c's settle and park, the 9P flush machine, the Go fork's os/signal names, viv-pheno-probe's helpers.
