---
id: adt-c10-r1
type: adt
title: "Each kernel stack in a range of its own, round 1"
date: 2026-11-03
scope: [sub-kernel-thread, sub-kernel-mmu, sub-kernel-exception, sub-kernel-halls]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 7}
findings: [fnd-c10-r1-f1, fnd-c10-r1-f2, fnd-c10-r1-f3, fnd-c10-r1-f4, fnd-c10-r1-f5, fnd-c10-r1-f6, fnd-c10-r1-f7, fnd-c10-r1-f8]
round-of: chg-2026-11-03-c10a-kstack-range
created: 2026-11-04
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, reading a frozen copy at `8258c389c`. Nothing had booted. No round 2 was owed: the fixes are one per-CPU word in the dump, two guards, witnesses, a flag and comments.

Withdrawn after prosecution: every holder of a stale stack pointer, every SP the kernel holds against the two bits, x0 inside SP, the release's order, the table's publish, the `kva_to_pa` census, the pool charge's bound, the slot map's arithmetic and the overflow stack's layout.
