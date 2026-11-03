---
id: fnd-aux-corona-join-r1-f1
type: fnd
round: adt-aux-corona-join-r1
severity: P3
status: fixed
title: "The pts interaction woke binding pollers under the process table lock"
surface: [sub-kernel-pts, sub-kernel-proc]
threatens: [inv-i9]
fixed-by: chg-2026-10-29-aux-corona-join
regression: "pts.interaction_wakes_after_the_table_lock; SJ3a (ACK path) and SJ3b (setsid path) RED on 009d20767"
created: 2026-11-02
---
## Prosecution

`proc_pts_interaction` and `pts_interaction_invalidate_locked` (from setsid, setpgid, both zombie paths and the exec swap) walked a binding's poll list under `g_proc_table_lock`, IRQs masked, and the list holds any number of hooks: the walk vote 13 removed ([[spec-poll-owed]]). Fixed in f3f5b4b7b: the bindings are listed in the holder's `struct pts_wakes` (u8 pool slots, each pinned) and woken after its release.
