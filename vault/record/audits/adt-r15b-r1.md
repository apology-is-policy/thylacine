---
id: adt-r15b-r1
type: adt
title: "r15-b — the refcount-atomicity round"
date: 2026-05-14
scope: [sub-kernel-pipe]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: clean
counts: {p0: 0, p1: 1, p2: 1, p3: 0}
findings: [fnd-r15b-f234]
round-of: chg-2026-05-14-r15b-atomic-refs
created: 2026-08-01
---
## Scope

Two refcounts, one race shape: the Spoor's (F233, P1 — the dev/spoor
surface, recorded there when swept) and the pipe ring's (F234, P2 —
[[fnd-r15b-f234]]). This record carries the round; the pipe finding
is the one on this batch's surface.

## Verdict

Clean; both made `__atomic` ACQ_REL with fetch-pre ownership. The
round predates SMP being ON — the value of fixing a torn RMW before
any CPU can tear it is that nobody ever had to debug it as a
Heisenbug.
