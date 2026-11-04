---
id: adt-c8a-r2
type: adt
title: "Torpor keyed on the space, round 2"
date: 2026-11-04
scope: [sub-kernel-torpor, sub-kernel-sched-smp]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 2, p3: 5}
findings: [fnd-c8a-r2-f1, fnd-c8a-r2-f2, fnd-c8a-r2-f3, fnd-c8a-r2-f4, fnd-c8a-r2-f5, fnd-c8a-r2-f6, fnd-c8a-r2-f7]
round-of: chg-2026-11-03-c8a-torpor
created: 2026-11-04
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 1c0b9c216, after e8b0515ee's first boot. Not a dirty close by the audit skill's rule: no P0, two P1 and P2, and small fixes. Re-derived and withdrawn: the pinned fix's memory order and its callers, the new extinction's callers, the hook's window and budgets, the two death tests' contracts, the wake's batch and hold, the key against exec and a vfork child, the refusal under a counted lock, the walks' lock order. Not read deeply: `file_demand_page_slow`, `burrow_map_fixed`, `proc_become_zombie_locked`, `timerwait_tick`, the job-stop fan.
