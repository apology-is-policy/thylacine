---
id: adt-rw2-wake-r1
type: adt
title: "HOLOTYPE RW-2, the wait/wake slice — and its dirty-close round 2"
date: 2026-06-10
scope: [sub-kernel-poll]
reviewer: opus
model-start: "opus-4.8-max"
model-end: "opus-4.8-max"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 0, p3: 1}
findings: [fnd-rw2-2cf1, fnd-rw2-r2poll-f1]
round-of: chg-2026-06-10-rw2-poll-retain
created: 2026-08-01
---
## Scope

The RW-2 review's poll slice (the full round also carried the
scheduler/death P1s recorded with the scheduling area). Counts here:
the poll slice only — 2C-F1 [P1] in round 1, R2-poll F1 [P3] in the
dirty-close round 2 on the fixes.

## Verdict

Dirty by the book: round 1's fix changed a lifetime protocol, so
round 2 prosecuted the fix itself — and found the retain INERT for
one handle kind ([[fnd-rw2-r2poll-f1]]). Round 2 closed clean
(0 P0/P1/P2 on the poll slice).

The pair demonstrates the re-audit discipline paying twice: the
round-1 finding closed a nine-days-old class
([[fnd-poll-r1-f3]]'s voided precondition), and the round-2 finding
caught the CLOSURE overclaiming — poll.h said "keeps a SrvConn alive
directly", which was false for listeners, and the falsity is exactly
what a mortal registry would weaponize
([[seam-poll-srv-registry-retain]]).
