---
id: adt-rw1-mm-r1
type: adt
title: "HOLOTYPE RW-1, the allocator slice"
date: 2026-06-10
scope: [sub-kernel-mm-slub]
reviewer: opus
model-start: "opus-4.8-max"
model-end: "opus-4.8-max"
verdict: clean
counts: {p0: 0, p1: 0, p2: 3, p3: 2}
findings: [fnd-rw1-af1, fnd-rw1-af2, fnd-rw1-fs1]
round-of: chg-2026-06-10-rw1-allocator
created: 2026-08-01
---
## Scope

The RW-1 holotype review's mm findings (the review itself ranged
wider; per-surface slices are recorded with their surfaces). Counts
here are the allocator slice only: A-F1 + A-F2 (2 P2) and
F-S1/F-S2/F-S3 (1 P2 + 2 P3).

## Verdict

Clean close, three P2 fixes. The slice's theme: everything the
Phase-1 allocator deferred as "runtime doesn't do that yet"
(runtime cache create/destroy, concurrent walkers, adversarial
sizes) had become reachable, and each economy was a finding.
[[fnd-rw1-fs1]] is the keeper — the existing `nr_full` guard is
what stopped anyone asking whether it was the RIGHT guard.
