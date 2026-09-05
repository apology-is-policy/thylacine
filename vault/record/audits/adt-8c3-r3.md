---
id: adt-8c3-r3
type: adt
title: "8c-3 round 3 (the stop_unwound latch) -- converged clean"
date: 2026-07-17
scope: [sub-kernel-ninep-client]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 1}
findings: [fnd-8c3-r3-f1]
round-of: chg-2026-07-17-8c3-reader-role
prior-round: adt-8c3-r2
created: 2026-07-31
---
## Scope

The stop_unwound latch lifecycle + the three classifier changes.

## Convergence

Clean: the latch is owner-only end-to-end (set in the detour, reset at recv
entry, read by the same thread; a concurrent resume touches only
debug_stop_req -- the race closed by construction). The P3 was doc-rot the
fix itself created; fixed in-close.
