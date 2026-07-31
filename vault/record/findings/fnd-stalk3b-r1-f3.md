---
id: fnd-stalk3b-r1-f3
type: fnd
title: "With the per-Proc cap removed, one Proc can hold all 64 global conn slots"
round: adt-stalk3b-r1
severity: P3
status: documented
surface: [sub-kernel-devsrv]
threatens: []
created: 2026-07-31
---
## Prosecution

Removing `SRV_CONN_PER_PROC_MAX` (the deliberate 3a-F4 decision — a
session needs corvus AND its stratum-fs concurrently) leaves only the
global `SRV_MAX_CONNS = 64` soft cap: one multi-thread Proc can hold
every slot, failing other Procs' `/srv` connects until it frees them.

## Disposition

Documented as an ACCEPTED cross-Proc fairness tradeoff — memory stays
bounded; correctness and isolation do not break; re-introducing a
per-Proc cap would contradict the F4 decision. Per-registry caps arrive
with the per-session-registry work ([[seam-srv-registry-lifecycle]]);
revisit fairness there.
