---
id: adt-8c3-r1
type: adt
title: "8c-3 (#89) holotype, round 1 (on the simple-unwind draft)"
date: 2026-07-17
scope: [sub-kernel-ninep-client]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 1}
findings: [fnd-8c3-r1-f1, fnd-8c3-r1-f2, fnd-8c3-r1-f3]
round-of: chg-2026-07-17-8c3-reader-role
created: 2026-07-31
---
## Scope

The reader-role release across a debug stop (the simple-unwind draft,
pre-commit): stop_unwinds, the top-of-loop park, the handoff skip. Rounds
ran on uncommitted drafts; all fixes landed in the single chunk commit.

## Convergence

REFUTED two self-audit verdicts with grounded findings: delivery is chunked
(F1 -> the frame-atomic redesign) and the role-release covered one of four
reader_active sites (F2). Dirty; the frame-atomic re-fix went to round 2.
