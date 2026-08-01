---
id: adt-signals13b-r1
type: adt
title: "P6-pouch-signals-b round 1"
date: 2026-05-24
scope: [sub-pouch-signal]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: dirty
counts: {p0: 0, p1: 2, p2: 6, p3: 5}
findings: [fnd-signals13b-r1-f1, fnd-signals13b-r1-f2, fnd-signals13b-r1-f10, fnd-signals13b-r1-f11]
round-of: chg-2026-05-24-p6-signals-b
created: 2026-08-01
---
Focused opus prosecutor over the nine-file signal patch + the one-line
kernel `pid=0` sentinel. DIRTY by the count rule (P1 + P2 = 8 >= 6), so a
round 2 was scheduled on the close.

A concurrent self-audit caught 10 of the same hazards and MISSED two --
F1 (the seam-check omission, which is the cross-chunk discipline from the
threads round, forgotten) and F11 (the SIG_ERR rejection) -- and rated F2
a P2 where the prosecutor's P1 was correct. Both misses are recorded in
the round's own summary.
