---
id: moc-kernel-ipc-wake
type: moc
title: "IPC wake consumers: poll, pipe, torpor"
parent: moc-kernel
created: 2026-08-01
updated: 2026-08-01
---
The consumers of [[moc-kernel-scheduling]]'s wait/wake primitive —
the three mechanisms that turn `sleep`/`tsleep`/`wakeup` into things
userspace can actually block on:

- **[[sub-kernel-poll]]** — one thread, N readiness sources behind N
  different locks. The `poll_waiter` hook is the cross-lock handoff;
  register-then-observe is the discipline.
- **[[sub-kernel-pipe]]** — the connected Spoor pair. Two
  single-waiter rendezes plus a poll list on one ring; every mutation
  that can enable a waiter carries its wake.
- **[[sub-kernel-torpor]]** — the futex. A user-VA word as the
  condition, a global-lock register-then-observe as the proof, and
  the death/stop cascades' completing and non-completing walks.

The area's shared grammar is [[inv-i9]] instantiated three ways: the
window between observing "not ready" and parking must be closed by
registering FIRST, under the same serialization the producer's wake
takes. poll closes it under each object's lock, pipe under the ring
lock, torpor under `torpor_lock` — and each then delegates the
park itself to the scheduler's modelled machinery
([[spec-scheduler]], [[spec-tsleep]]).

The area's shared history is the multi-thread lift: all three were
born under single-thread-per-Proc assumptions that P6-pouch-threads
voided. poll's borrow became a UAF ([[fnd-rw2-2cf1]]), pipe's ref
became a torn RMW ([[fnd-r15b-f234]]), torpor grew the die-pending
re-check. The dossiers carry the per-surface closes.

Sibling wake consumers live with their owners: the console's deferred
poll-wake in console-gfx, dev9p.poll and srvconn in ninep/srv, notes
delivery in execution.

Specs: [[spec-poll]] · [[spec-pipe]] (+ [[spec-tsleep]] below).
Locks: [[lock-poll-list]] · [[lock-pipe-ring]] · [[lock-torpor]].
