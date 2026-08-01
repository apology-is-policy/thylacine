---
id: adt-pty3-r1
type: adt
title: "PTY-3 round 1 (the pouch pty boundary-line)"
date: 2026-07-18
scope: [sub-pouch-tty]
reviewer: fable
model-start: "claude-fable-5"
model-end: "claude-fable-5"
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 3}
findings: [fnd-pty3-r1-f1, fnd-pty3-r1-f2, fnd-pty3-r1-f3]
round-of: chg-2026-07-18-pty3
created: 2026-08-01
---
Fable-5-max holotype (MODEL start == end, no fallback) + a concurrent
self-audit. The two prosecutors CONVERGED on F3; the formal round
additionally caught F1 (helper-generic sizing, which the self-audit's
call-site count had reasoned away) and F2.

Its sound set is unusually load-bearing and worth reading whole: the
`openat` stalk migration's errno delta, the trailing-slash and bare-`/`
behavior, the S_IFCHR posture's kernel-inertness, the two-gate
discrimination against `/net`'s bit-40 qids, the tty receive-only gate's
completeness, and the SIG_DFL matrix.
