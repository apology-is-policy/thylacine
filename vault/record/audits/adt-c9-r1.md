---
id: adt-c9-r1
type: adt
title: "The switch decides from TTBR0, round 1, before the first boot"
date: 2026-11-04
scope: [sub-kernel-sched-smp, sub-kernel-thread]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 5}
findings: [fnd-c9-r1-f1, fnd-c9-r1-f2, fnd-c9-r1-f3, fnd-c9-r1-f4, fnd-c9-r1-f5]
round-of: chg-2026-11-04-c9-switch
created: 2026-11-04
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 370a5b5ea. Five P3s: two on the record's figures and the sabotage fixture, three on wording and a witness nothing can supply. The tests are judged on reading: this code has not been built or booted.

Re-derived and withdrawn: every writer of TTBR0, so a thread that returns to EL0 never runs on ASID 0; every reader and writer of a thread's V state; first dispatch; the equal-value skip against I-48 and a reused root; the rollover between save and load; the assembly's registers and offsets. Not read deeply: the full exit path, loom's sqpoll body, KERNEL_ENTRY and KERNEL_EXIT beyond a grep.
