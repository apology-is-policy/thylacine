---
id: adt-349-self
type: adt
title: "#349 concurrent self-audit (pre-formal)"
date: 2026-06-24
scope: [sub-kernel-ninep-client]
reviewer: self
model-start: "claude-opus-4-8 (the implementing session)"
model-end: "claude-opus-4-8 (the implementing session)"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 0, p3: 0}
findings: [fnd-349-self-sa1]
round-of: chg-2026-06-24-349-flow-control
created: 2026-07-31
---
## Scope

The audit-in-flight self-prosecution run concurrently with the formal
round: the park branch, EAGAIN collision-freedom, cleanup balance,
self-pump re-entrancy, deadlock-freedom.

## Convergence

Found SA-1 (P1, fixed pre-close). NOTE the instructive miss: the
self-audit's "follows the rpc->done pattern" reasoning cleared the park --
the formal round then showed that exact reasoning wrong (fnd-349-r1-f1).
