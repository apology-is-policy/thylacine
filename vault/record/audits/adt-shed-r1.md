---
id: adt-shed-r1
type: adt
title: "The mount-table shed round 1: a union root sheds its own entries, and the spec that should have caught it could not fail"
date: 2026-09-21
scope: [sub-kernel-territory, sub-kernel-spoor, sub-kernel-dev, sub-kernel-joey, sub-stratum-boot]
reviewer: fable
model-start: "claude-fable-5-1"
model-end: "claude-fable-5-1"
verdict: dirty
counts: {p0: 0, p1: 1, p2: 1, p3: 6}
findings: [fnd-shed-r1-f1, fnd-shed-r1-f2]
round-of: chg-2026-09-21-mount-shed
created: 2026-09-21
---
## Scope

Branch `browser-b0` @ 9f7613f0: `territory_shed_unreachable_locked` and its two call sites, `Dev.devno_per_walker`, joey's `/hw/pci` re-graft, `specs/territory_shed.tla`, eight kernel tests, the scripture and four dossiers. Read-only; TLC on scratch copies and a host replay of the algorithm.

## Convergence

The closure and the compaction are correct as coded; the PREMISE was not. "Every Spoor a resolution from the new root can hold is in R" is false when the root is a union directory handle: the resolver consults the union's mount point, which lives in another tree, without having walked to it ([[fnd-shed-r1-f1]]). The spec could not have caught it because its soundness invariant compared the rule with itself and held for any rule at all ([[fnd-shed-r1-f2]]). The author's concurrent self-audit re-derived the unmount parity, the fd-relative change and the devno assigner sets, found nothing, and missed F1 -- it asked what a resolution can HOLD and never what the resolver CONSULTS at the base. The round also verified a long list sound (the assigner sets, the hostile-server bound, lock discipline, refcounts, I-1, MNOEXEC + I-43), recorded with the six P3s in `memory/audit_territory_shed_closed_list.md`. Recorded `dirty` because the P1 fix moves the closure seed and rewrites the spec; a second round follows.
