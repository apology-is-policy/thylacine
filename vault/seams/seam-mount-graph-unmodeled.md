---
id: seam-mount-graph-unmodeled
type: seam
title: "The live mount-cycle check has no model; NoCycle proves the dead bind table"
status: open
surface: [sub-kernel-territory]
opened-by: fnd-stalk2-r1-f1
tracker: "unfiled"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`specs/territory.tla::NoCycle` — the only cycle invariant in the model —
ranges over `bindings`. At v1.0 that table is dead: no `SYS_BIND`
exists, `bind()` has no production caller, and the resolver never reads
it. The graph that IS live is the mount identity graph, guarded by
`would_create_mount_cycle`, and it is unmodeled.

The asymmetry is not academic. `would_create_mount_cycle` exists only
because [[fnd-stalk2-r1-f1]] falsified the standing claim that [[inv-i3]]
held "by construction" on the mount table — the exact class of
assumption a model exists to break. The check that replaced the false
claim is now protected by one unit test and a prosecution row.

## What closes it

Extend `territory.tla` so `mounts` carries the mount-POINT identity
(rather than an abstract `path`) and a `MountNoCycle` invariant ranges
over the mount edges, with a `BUGGY_MOUNT_CYCLE` cfg as the
counterexample. Cheap — the algorithm is the one already modeled for
binds, applied to the other variable.

The honest alternative, if the bind table is deleted rather than
revived: `NoCycle` MOVES to the mount graph rather than being joined by
a sibling, and the spec loses a variable instead of gaining one.

## Risk while open

Low today and self-limiting: a cyclic mount is rejected at insertion, so
the failure mode of a regressed check is a `stalk` that resolves a name
to a wrong endpoint by going around the loop — bounded by
`STALK_MAX_DEPTH`, so it terminates. The real risk is drift: a future
change to the mount keying (the union-mount work is the likely one)
would be spec-checked for refcounts and NOT for cycles.
