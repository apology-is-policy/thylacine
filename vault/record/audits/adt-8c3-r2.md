---
id: adt-8c3-r2
type: adt
title: "8c-3 round 2 (on the frame-atomic re-fix)"
date: 2026-07-17
scope: [sub-kernel-ninep-client]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 0, p3: 1}
findings: [fnd-8c3-r2-f1, fnd-8c3-r2-f2]
round-of: chg-2026-07-17-8c3-reader-role
prior-round: adt-8c3-r1
created: 2026-07-31
---
## Scope

The frame-atomic restructure: flag lifecycle, block-through, four-site stop
handling, the send_flow spill-before-park, death-wins, lock order.

## Convergence

The self-audit missed F1 again (the classifier race vs an async resume) --
the two-prosecutor discipline earning its keep twice in one chunk. Dirty
(a P1 on the fix); recursion round 3 followed.
