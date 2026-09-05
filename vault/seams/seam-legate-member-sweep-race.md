---
id: seam-legate-member-sweep-race
type: seam
title: "A legate member spawned racing the teardown walk is missed"
status: open
surface: [sub-kernel-proc]
opened-by: chg-2026-08-01-proc-thread-sweep
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

A strict whole-subtree close on legate scope teardown. The walk
group-terminates every Proc carrying the dying root's `legate_scope_id`, but
a member spawned concurrently with the walk can be missed.

## What closes it

An `rfork`-under-lock parent-flag check — refuse (or enrol) a child whose
parent is in a scope currently being torn down. Recorded in the code as a
v1.x tidiness refinement.

## Risk while open

Deliberately low, and the argument is worth preserving because it explains
why this is tidiness rather than a hole: at v1.0 the clearance set is ALL
elevation-only, and `rfork` strips exactly those, so a scope MEMBER never
holds the elevated caps — only the ROOT does. I-25's privilege guarantee
therefore rests on the root, which dies on its own exit or self-terminates
at `valid_until` expiry.

A missed member is an UNELEVATED straggler carrying a stale scope tag. Not
an invariant violation; just untidy.
