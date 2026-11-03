---
id: adt-thread-c7-r1
type: adt
title: "Thread-model C7 (one first entry, per-thread /proc, readdir) round 1"
date: 2026-10-29
scope: [sub-kernel-exception, sub-kernel-thread, sub-kernel-devproc, sub-kernel-poll, sub-kernel-ninep-dev9p-poll, sub-kernel-syscall-dispatch]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 1, p2: 0, p3: 4}
findings: [fnd-thread-c7-r1-f1, fnd-thread-c7-r1-f2, fnd-thread-c7-r1-f3, fnd-thread-c7-r1-f4, fnd-thread-c7-r1-f5]
round-of: chg-2026-10-29-thread-model-c7
created: 2026-10-29
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, read-only over git objects at 4d2c3b15e (base ca7ba9eca; the working tree was being rewritten by sabotage runs). Not dirty (no P0, P1 + P2 = 1, a one-line fix), but F1 led to operator vote V5, whose native-arm change got round 2. Verified sound: the first-entry frame, the spec, every per-thread gate (each file reuses its Proc-level callback), the poll residue tests.
