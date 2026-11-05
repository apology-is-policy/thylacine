---
id: adt-c9-r2
type: adt
title: "The switch decides from TTBR0, round 2: kernel threads on a real ASID 0"
date: 2026-11-05
scope: [sub-kernel-sched-smp, sub-kernel-thread, sub-kernel-mmu]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 4}
findings: [fnd-c9-r2-f1, fnd-c9-r2-f2, fnd-c9-r2-f3, fnd-c9-r2-f4]
round-of: chg-2026-11-05-kernel-root-pa
created: 2026-11-05
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 64f75eba9. Round 1 read the switch on the premise that a kernel thread runs on ASID 0, which `chg-2026-11-05-kernel-root-pa` shows was false until then, so nothing had ever run in the state it reasoned about. No defect in C9's diff; four P3s, one of them a pre-existing I-31 contract violation in a test fixture.

Re-derived from code: every reader and writer of a Context's TTBR0 and of the live register, with nothing depending on the old value; the identity's flush against `smp_init`'s order, its entries being global, and no writer of `l2_ttbr0` after it; a kernel thread across a rollover, which leaves the last user cid in the slot, where it is reserved and the flush honoured on that CPU's next user switch; every thread's V decision, including exec, exit and fork; no path to EL0 from a thread switched in on ASID 0; the census of pre-branch stores of a link-time address. `lib/dtb.c` was missing from the frozen copy: the implementer closed that gap by grep, finding one global store there, a walked count.

A kernel thread touching a low VA was an address-size fault on the junk root and is a level 2 translation fault now, so uaccess from a created kernel thread would have died before; none exists in-tree.
