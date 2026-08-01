---
id: seam-proc-find-no-refcount
type: seam
title: "proc_find_by_pid returns an unrefcounted pointer"
status: open
surface: [sub-kernel-proc]
opened-by: chg-2026-08-01-proc-thread-sweep
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

Proc refcounting. `proc_find_by_pid` acquires [[lock-proc-table]], walks,
and returns a bare `struct Proc *` after RELEASING it. The header is candid:
the pointer is stable only under a "no concurrent reap" assumption, and
Phase 5+ was supposed to return a `proc_get`'d reference requiring a
`proc_put`.

## What closes it

A refcount on `struct Proc` with acquire-at-lookup / release-at-done, which
is a wider change than it looks — the reap path currently frees on the
strength of the ZOMBIE state alone.

## Risk while open

Contained today, and the containment is worth naming because it is the
pattern to copy rather than the pointer to trust: every consumer either
holds the table lock across the whole use (`proc_for_each` callbacks) or
lets only VALUES escape. `proc_peer_snapshot_by_stripes` is the model —
it snapshots caps, identity, pid and role under the lock and returns no
pointer at all, so a peer reaped after the scan is not a use-after-free.

A future caller that keeps the returned pointer past the lock is the hazard,
and nothing in the signature stops it.
