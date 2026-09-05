---
id: adt-net5-r1
type: adt
title: "net-5 round 1 (the AF_INET boundary-line)"
date: 2026-06-18
scope: [sub-pouch-net]
reviewer: opus
model-start: "claude-opus-4-8"
model-end: "claude-opus-4-8"
verdict: clean
counts: {p0: 0, p1: 0, p2: 1, p3: 3}
findings: [fnd-net5-r1-f1]
round-of: chg-2026-06-18-net5-af-inet
created: 2026-08-01
---
Opus-4.8-max prosecutor (MODEL start == end) + a concurrent self-audit,
which CONVERGED on both the sound set and the dispositions. All four
findings are fail-closed-safe -- loud `ENOSYS` or unreachable latents --
which is the tag design doing its job.

Its recorded LESSON is the one that generalizes: a feature landing
invalidates the "feature absent" test. The prover asserted
`socket(AF_INET)` is refused (a 0006-era check); net-5 made it valid, so
the boot failed on a stale assertion -- and the failure went to stderr,
not the fd-1 pipe, so only the header showed.
