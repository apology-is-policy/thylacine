---
id: adt-shed-r2
type: adt
title: "The mount-table shed round 2: the seed is complete and the spec can fail; a union handle is a latent capability on the directory it covers"
date: 2026-09-21
scope: [sub-kernel-territory, sub-kernel-stalk, sub-kernel-spoor]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 0, p2: 2, p3: 5}
findings: [fnd-shed-r2-f1, fnd-shed-r2-f2]
round-of: chg-2026-09-21-mount-shed
prior-round: adt-shed-r1
created: 2026-09-21
---
## Scope

Branch `browser-b0` @ 1de0692a: the round-1 scripture and fixes. Read-only. The reviewer re-derived all seven TLC verdicts, ran eleven spec sabotages, and replayed the REAL shed code, `sed`-extracted from the tip, over 300,000 random tables against an independent instance-level truth (0 unsound, 0 residue; controls discriminate: no seed -> 48,497 unsound).

## Convergence

The round-1 fixes are correct as far as they go: the seed set is complete for root-relative resolution, the bound is exact, and the spec fails for every rule fault injected. The finding sat one step to the side again. A union handle keeps the directory the union was mounted OVER, and once the union's entries are gone -- a plain `unmount("/")` loop does it, no shed needed -- `"/"` of a union root and `"."` of a union dirfd returned that covered directory ([[fnd-shed-r2-f1]]). The shed added a second way to get there. And the obligation that caused round 1's P1 was written down on the shed's side only, where the next resolver change would never look ([[fnd-shed-r2-f2]]). Recorded `dirty` because the fix edits the resolver's base handling; a third round follows.
