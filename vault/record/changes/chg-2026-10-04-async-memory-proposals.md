---
id: chg-2026-10-04-async-memory-proposals
type: chg
title: "Propose reusable async connection and memory-accounting lifecycles"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-loom, sub-kernel-mm-phys, sub-kernel-addrspace]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The operator requested both designs. ASYNC-SERVICE-LIFECYCLE proposes Loom
private scopes and peer-independent retirement; SHARED-MEMORY-ACCOUNTING proposes
durable funding/retention accounts, class-aware capacity and pressure recovery.
ASYNC-MEMORY-DESIGN-REVIEW identifies binding choices, source fit, prior art,
implementation stages and single-agent design review. Ratification is pending.

No ABI numbers are reserved and no new mechanism is implemented. Existing limits,
protected authority drafts and nondefault clipboard activation remain unchanged.
Research/source inspection does not qualify concurrency, allocation rollback,
hardware fences or application behavior; the specifications require those gates.
