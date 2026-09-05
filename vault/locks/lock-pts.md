---
id: lock-pts
type: lock
title: "g_pts_lock — the pseudoterminal registry"
kind: spin
guards: "the whole 64-entry pts registry: liveness, generation, minting server, the connection/qid bindings, and the controlling-terminal pair (session, foreground group)"
orders-before: []
created: 2026-08-03
updated: 2026-08-03
---
## Discipline

A **leaf**, and unusually strict about it — the interesting content of this
lock is not what it nests but what it refuses to.

Two things happen under it and nothing else does:

- `srvconn_ref` — an atomic increment, which takes no lock.
- `srvconn_is_live` — a magic check and a single read of a one-way
  `LIVE → TORN` field, which takes no lock either.

Everything with weight is **staged under the lock and run after it**:

- Dropping a connection reference. The last unref tears the connection down
  and frees it, which takes the channel and slab locks. So a clear collects
  its bindings' connections into a small stack array, returns a count, and
  the caller unrefs them after release. See [[lock-srvconn-chan-lock]].
- Posting a note, walking the process table, resuming a stopped group.
  Every one of those takes [[lock-proc-table]].

## Why the process-table lock must never nest under it

This is the load-bearing rule, and it shapes three separate call sites.

The signal path needs the controlling session and the foreground group,
which live in the registry, and then needs to post to a process group, which
needs the process table. Nesting would be the obvious implementation. Instead
the registry read **snapshots** `(ct_sid, fg_pgid)` under the lock, releases,
and fans out on the snapshot.

That is not merely lock hygiene — it changes the semantics, and the change is
argued rather than accidental: a concurrent free-and-remint cannot redirect an
in-flight signal, because the foreground group was captured while the id was
still valid. A post to a group that emptied inside the window is the ordinary
POSIX race, indistinguishable from the signal arriving a moment earlier.

The same shape appears twice more. The teardown fan runs on a staged snapshot
after release. And the membership gate for seating a foreground group runs
**before** the lock is taken, unlocked, precisely because it walks the process
table — accepting a benign race rather than inverting the order.

## What the strictness costs

The one place the discipline bites is acquisition. Deciding whether a
terminal's recorded controlling session is still *alive* would need the
process table, under this lock, at the point of decision. It is not done, so
a dead session's claim on a terminal outlives it — see [[sub-kernel-pts]]'s
caveats and task #67. The unlocked-precheck shape used by the foreground-group
gate is available here too; it simply was never applied.

## Prosecution

- Nothing that can sleep, allocate, or take the process-table lock may run
  under this lock.
- Every connection unref stays staged and post-release; a clear that unrefs
  in place is a lock-order inversion into the slab.
- The snapshot-then-fan shape is a correctness argument, not a style — a
  future "just hold the lock across the post" collapses it.
