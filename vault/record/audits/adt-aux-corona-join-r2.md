---
id: adt-aux-corona-join-r2
type: adt
title: "The corona join round 2: the round-1 fixes and the protocol index's charge"
date: 2026-10-29
scope: [sub-kernel-pts, sub-kernel-proc, sub-kernel-poll, sub-kernel-loom]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 4}
findings: [fnd-aux-corona-join-r2-f1, fnd-aux-corona-join-r2-f2, fnd-aux-corona-join-r2-f3, fnd-aux-corona-join-r2-f4]
round-of: chg-2026-10-29-aux-corona-join
created: 2026-11-02
---
holotype-reviewer on Fable 5.1 on f3f5b4b7b and 842c0c908, read-only while the gates ran. Withdrawn as guarded: a binding listed twice or a `pts_wakes` overflow (one `_locked` call per batch, one listing per slot), a slot reused before its wake (`wake_refs` holds `used`), a wake lost across the deferral (register, sample and every bump are under `g_pts_lock`), the Proc touched after the release (`pti_wake` reads only `g_pti`), a walk inside a Dev.poll. Verified: every `++wake_refs` reaches one `pti_wake`; the index's charge equals its allocation, is refunded symmetrically and unwinds on the fault paths; no other struct aux-3 grew sits in an unguarded fixed-size allocation. Noted, not prosecuted: the spawn-rollback ZOMBIE stores skip `pts_interaction_invalidate_locked`, and a binding naming such a child fails closed through `proc_pts_live_locked`.
