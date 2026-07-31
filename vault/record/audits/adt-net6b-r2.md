---
id: adt-net6b-r2
type: adt
title: "net-6b round 2 (on the F1 pump restructure)"
date: 2026-06-18
scope: [sub-kernel-ninep-dev9p-poll]
reviewer: opus
model-start: claude-opus-4-8
model-end: claude-opus-4-8
verdict: clean
counts: {p0: 0, p1: 0, p2: 0, p3: 2}
findings: [fnd-net6b-r2-f1, fnd-net6b-r2-f2]
round-of: chg-2026-06-18-net6b4-close
prior-round: adt-net6b-r1
created: 2026-07-31
---
Both P3s were SHARPENINGS of round-1's documented seams (the >16-client
LIFO cliff named as starvation-not-latency; the narrower-live-op OOM
no-progress folded into the F2 degrade). CONVERGED CLEAN over 2 rounds.
