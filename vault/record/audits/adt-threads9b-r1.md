---
id: adt-threads9b-r1
type: adt
title: "P6-pouch-threads-b round 1"
date: 2026-05-23
scope: [sub-pouch-thread]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: clean
counts: {p0: 0, p1: 2, p2: 3, p3: 10}
findings: [fnd-threads9b-r1-f1, fnd-threads9b-r1-f2, fnd-threads9b-r1-f5]
round-of: chg-2026-05-23-p6-threads-b
created: 2026-08-01
---
Focused opus prosecutor over the eight-file pthread patch, the post-patch
musl tree, the proving binary, and the build wiring; the kernel-side
contracts were re-read but not re-audited (the 9a closed list covers
them).

Its own confidence notes name what it could not reach: the C11
`thrd_create` path, cross-Proc shared memory, robust-mutex behavior under
crash, and thundering-herd performance after the requeue removal. One
finding (S1, a timeout-overflow guard) was self-found and pre-folded
before the round, then superseded by F1's clamp -- which is the healthier
outcome: the deeper fix made the defensive one moot.
