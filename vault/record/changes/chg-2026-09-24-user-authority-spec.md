---
id: chg-2026-09-24-user-authority-spec
type: chg
title: "Specify scoped user administration and Imperium transactions"
date: 2026-09-24
arc: arc-user-authority
commits: ["*(pending)*"]
touched: [sub-corvus, sub-imperium, sub-kernel-caps]
established: [dec-2026-09-24-user-authority-direction]
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-24
---
Write the requested implementation specification: symbolic administrative
operations, persistent grant envelopes/provenance, typed temporary Admin scopes,
Imperium command grammar, trusted transaction and storage contracts, live
revocation, installer/recovery and implementation/acceptance stages. Explicitly
record proposed supersessions of the old mandate design without claiming new
behavior or ratified numeric ABIs. Track the existing debug-taint and non-bit
cover gaps as UA-P0/P1 blockers. Review is single-agent specification review;
Vault lint checks graph structure, not correctness of the proposed mechanisms.
