---
id: adt-c8b-timer-bases-r1
type: adt
title: "C8b round 1, before the first boot"
date: 2026-11-03
scope: [sub-kernel-rendez, sub-kernel-sched-smp, sub-kernel-sched, sub-kernel-thread]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 4}
findings: [fnd-c8b-timer-bases-r1-f1, fnd-c8b-timer-bases-r1-f2, fnd-c8b-timer-bases-r1-f3, fnd-c8b-timer-bases-r1-f4]
round-of: chg-2026-11-03-c8b-timer-bases
created: 2026-11-03
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at f8423a407, scope a86f1774e..f8423a407, compiled and model-checked but not booted. It built the interleavings of the tick against the owner on stack and heap Rendezes, re-sleep on the same and another r, the on_cpu filter, lock-order cycles with two ticks, an owner and the death cascade, the secondary on cpu0's base, and tickless parks, and could break none. It re-ran TLC on its own copy and reproduced the SPEC-TO-CODE table, and read unlink_early's trace as the real use-after-free shape. It did not re-derive sched_tickless.tla against the per-CPU `earliest`, nor read poll.c's cond bodies, whose constraint keeps its shape. READ, NOT RUN.
