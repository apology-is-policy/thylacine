---
id: adt-c8a-r3
type: adt
title: "Torpor keyed on the space, round 3: the walks' removal and the ready_on guard"
date: 2026-11-04
scope: [sub-kernel-torpor, sub-kernel-death, sub-kernel-sched-smp]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-c8a-r3-f1, fnd-c8a-r3-f2, fnd-c8a-r3-f3]
round-of: chg-2026-11-03-c8a-torpor
created: 2026-11-04
---
holotype-reviewer on Fable 5.1, cross-family against the Opus implementer, on a frozen copy at 3e2adf489. Owed because the audit rule treats a removed primitive and a changed wake protocol as invasive, and round 2 had not seen them. Clean: the three findings are stale text.

Re-derived and withdrawn: every window where a waiter is linked and unnamed, against death, stop and the interrupt latch; what the walk gave that the cascade does not, including AWOKEN against INTR at both callers; exec and session hangup; the lock order after removal; every `ready_on` caller with a pinned thread and the guard's memory order, so a legitimate wake cannot extinct; the ungated IPI; `SleepCommit` against `tsleep_common`'s order. Not read deeply: `wake_rendez_waiter` past its spin, `notes_handoff_target`, `gic_send_ipi` against secondary bring-up, `el0_stop_park`, `debug_stop.tla` and `death_wake.tla` beyond their actions.
