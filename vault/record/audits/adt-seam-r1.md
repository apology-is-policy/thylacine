---
id: adt-seam-r1
type: adt
title: "P6-pouch-syscall-seam round 1"
date: 2026-05-22
scope: [sub-pouch-seam]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: clean
counts: {p0: 1, p1: 0, p2: 1, p3: 6}
findings: [fnd-seam-r1-f1, fnd-seam-r1-f6]
round-of: chg-2026-05-22-p6-syscall-seam
created: 2026-08-01
---
Focused opus prosecutor over the two seam patches + the `build_sysroot`
wiring. Counts as landed: 1 P0 / 1 P1 / 2 P2 / 5 P3, with the P1 and one
P2 DOWNGRADED to P3 during disposition (the not-a-tty probe and the
generic `EIO` are both correct-and-documented rather than
silently-wrong), and one finding WITHDRAWN by the audit itself after
prosecution proved the short-write accounting correct. Recorded here at
the dispositioned counts.

The P0 is the round's whole value: the guards were on the path everyone
reads and absent from the one nobody does.
