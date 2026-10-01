---
id: chg-2026-10-01-srv-listener-retention
type: chg
title: "Listener handles and poll snapshots retain their service registry"
date: 2026-10-01
arc: arc-astra-halcyon-followup
commits: ["*(pending)*"]
touched: [sub-kernel-devsrv, sub-kernel-handle, sub-kernel-poll]
established: []
closed: [seam-poll-srv-registry-retain]
opened: []
mirrors-checked: []
depth: skeletal
---
Listener slots and handle snapshots now keep the registry allocation alive,
including poll's existing retain-through-unregister path. The focused lifetime
and rollback witnesses reject three intended source mutants; the complete
1830-test boot and all 50 default/UBSan SMP boots pass. This closes the inert
poll-retain seam while [[seam-srv-registry-lifecycle]] remains open for capacity,
session namespace routing, poster death and fairness. Review is single-agent.
