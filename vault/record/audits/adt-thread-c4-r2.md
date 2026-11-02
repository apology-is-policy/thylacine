---
id: adt-thread-c4-r2
type: adt
title: "Thread-model C4 round 2 (the r1 fixes)"
date: 2026-11-02
scope: [sub-kernel-notes, sub-kernel-proc, sub-kernel-thread, sub-kernel-rendez, sub-kernel-vivarium, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-pouch-thread]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 2}
findings: [fnd-thread-c4-r2-f1, fnd-thread-c4-r2-f2]
round-of: chg-2026-11-02-thread-model-c4
created: 2026-11-02
---
Fable 5.1 on ce6e929a1 in a frozen worktree: 0/0/0/2 and two observations. Not dirty; closes the rounds. OBS-1, moved notes count against admission until read, fixed in docs. OBS-2, the model's ghost is monotone, was already stated. It enumerated the stores that turn a disposition fatal independently and found the same three. Read least deeply: the arch EL0 tail past .Lel0_return, proc_stop_sleeper_park's body, poll.c's settle and park, the 9P flush machine, cons.c past its NOTEINTR sites, the probe's helpers, viv_clone_thread.
