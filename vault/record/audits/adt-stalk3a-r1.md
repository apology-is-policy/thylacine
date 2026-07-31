---
id: adt-stalk3a-r1
type: adt
title: "stalk-3a round 1 (the namespace-resident registry)"
date: 2026-06-02
scope: [sub-kernel-devsrv]
reviewer: opus
model-start: "opus (2026-06 tier; exact id not recorded in the round log)"
model-end: "opus (2026-06 tier; exact id not recorded in the round log)"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 4}
findings: [fnd-stalk3a-r1-f1, fnd-stalk3a-r1-f2, fnd-stalk3a-r1-f3, fnd-stalk3a-r1-f4]
round-of: chg-2026-06-02-stalk3a-registry
created: 2026-07-31
---
## Scope

The registry heap-lift: `srv_registry_create/ref/unref` + the
attach/walk aux-ownership discipline + the boot-mount refcount trace +
the wrapper-over-`_in` parity. Opus prosecutor + an in-session
self-audit, CONVERGED on SOUND.

## Convergence

The verified-SOUND set (do-not-re-prosecute): the walk's aux-normalize
across every failure path (no phantom unref, no leak); the
clone_walk_zero interaction; the last-drop drain-outside-the-lock; the
boot-mount immortality trace (kproc's mount holds the Spoor-ref floor
forever); KOBJ_SRV obj can never be a registry (magic partition); lock
leafness + atomic ref; the two new lifecycle tests non-vacuous. All four
findings P3: one fixed in the close (devno), one deferred as the
mortal-registry prerequisite, one documented contract, one forward note
closed by stalk-3b's cap removal.
