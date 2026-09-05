---
id: chg-2026-06-10-rw2-poll-retain
type: chg
title: "RW-2 2C-F1: the poll waiter outlives the obj ref — retain across the sleep"
date: 2026-06-10
arc: arc-holotype-rw
commits: ["e504e8b2"]
touched: [sub-kernel-poll]
established: []
closed: []
opened: [seam-poll-srv-registry-retain]
mirrors-checked: []
depth: rich
---
## What

The RW-2 fix-class commit's poll slice (the commit also carried the
sched/death fixes recorded with the scheduling area). 2C-F1 [P1]
([[fnd-rw2-2cf1]]): a registered `poll_waiter` sits on the polled
OBJECT's embedded list for the whole sleep; with multi-thread Procs a
sibling can close the last handle mid-sleep, freeing the object AND
its embedded list — the sweep then spin-locks freed memory. This is
[[fnd-poll-r1-f3]]'s doc-fixed precondition come due.

The fix: `poll_scan_one` RETAINS the #844 `handle_get` obj ref
whenever it actually registered (`pw->list != NULL`), transferring it
to a `held[]` slot; the sweep releases all retained refs AFTER the
unregister pass. Transitively sufficient for both real registering
paths (pipe ring, devsrv connection — each frees its list only at
the Spoor's last clunk).

## Round 2's asterisk

The dirty-close round-2 found the retain INERT for the KObj_Srv
LISTENER ([[fnd-rw2-r2poll-f1]], P3): `handle_acquire_obj` no-ops on
that kind, so listener-poll safety rests on the boot registry's
immortality — a mortal per-session registry revives the UAF.
Comment-fixed + tracked: [[seam-poll-srv-registry-retain]].
