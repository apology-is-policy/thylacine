---
id: inv-i3
type: inv
title: "I-3 — mount points form a DAG, never a cycle"
number: I-3
guards: [sub-kernel-territory]
validated-by: [spec-territory, gate-smp]
strength: spec
created: 2026-08-01
updated: 2026-08-01
---
## Statement

A Territory's composition graph is acyclic. Adding an edge that would
close a loop is REJECTED at insertion, not detected during traversal —
so a resolver walking the namespace can never spin, and can never
resolve a name to a silently-wrong endpoint reached by going around a
cycle.

## Enforcement

Two checks in `kernel/territory.c`, same algorithm, different graphs:

- `would_create_cycle` on the bind graph — fixed-point reachability
  from `src` over existing `dst -> src` edges; if `dst` becomes
  reachable, the new edge would close `src -> ... -> dst -> src`.
  `bind` returns `-1`; the degenerate self-bind returns `-4`.
- `would_create_mount_cycle` on the MOUNT identity graph — the same
  fixed point over `(dc, devno, qid.path)` keys, rejecting a self-mount
  (source identity == mountpoint identity) or a cross-tree oscillation.
  `mount` returns `-3`, which the SVC layer collapses to `-1`.

Both run under [[lock-territory-ns-lock]], inside the same critical
section as the insertion they guard — a check-then-insert with the lock
dropped between would be the classic gap.

## Validation

[[spec-territory]]: `NoCycle` plus its `territory_buggy.cfg`
counterexample (bind without the check; two binds compose into a loop).
Runtime: `territory.cycle_rejected` (the chain plus the self-bind) and
`territory_mount.rejects_cycle`.

**blind-to:** the spec's `NoCycle` ranges over the BIND graph only, and
at v1.0 that table is dead — nothing populates it, no `SYS_BIND` exists
(see [[sub-kernel-territory]] Caveats). The live mount-graph check is
therefore unmodeled: it was added at stalk-2 precisely because I-3 had
been assumed to hold "by construction" on the mount table and did not
([[fnd-stalk2-r1-f1]]), and only a unit test and prose protect it now.
[[seam-mount-graph-unmodeled]] owes the model. The cross-Territory case
is out of scope by construction — the graph is per-Proc, so a cycle
cannot span two namespaces.
