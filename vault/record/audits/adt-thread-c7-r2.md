---
id: adt-thread-c7-r2
type: adt
title: "Thread-model C7 round 2 (the r1 close and vote V5)"
date: 2026-10-29
scope: [sub-kernel-notes, sub-kernel-syscall-dispatch, sub-kernel-devproc, sub-pouch-thread, sub-haul]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-thread-c7-r2-f1, fnd-thread-c7-r2-f2, fnd-thread-c7-r2-f3]
round-of: chg-2026-10-29-thread-model-c7
created: 2026-10-29
---
Scope be384162f..4f9c067f1 (read-only git objects): the clone and native mask copies, pouch 0065, the new tests, haul and libthyla-rs, the Go fork's minit read-only. Verified sound: publication ordering of a newborn's mask, the refused admit, kill and snare not deferrable, spawn from inside a handler, the latches, kernel threads untouched, 0065's window, haul's pumps, the Go runtime, no native consumer needing an unmasked newborn, test discrimination, the fixture's safety, F4's withdrawal. Clean.
