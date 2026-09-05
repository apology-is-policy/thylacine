---
id: adt-90-r1
type: adt
title: "#90 death block-through, round 1 -- clean"
date: 2026-07-19
scope: [sub-kernel-ninep-client]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 1}
findings: [fnd-90-r1-f1]
round-of: chg-2026-07-19-90-death-block-through
created: 2026-07-31
---
## Scope

The #90 arc: scripture, reader_frame.tla, the four guarded die-check sites,
the three regressions. A concurrent self-audit + a guard-completeness
cross-check ran alongside; all three converged.

## Convergence

SOUND and COMPLETE (guard completeness exhaustively confirmed: exactly four
thread_die_pending sites, all guarded, no fifth reachable mid-frame). Not
dirty; the P3 fixed in-close, revert-probed per-guard.
