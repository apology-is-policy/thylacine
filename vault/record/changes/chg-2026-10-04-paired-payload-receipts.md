---
id: chg-2026-10-04-paired-payload-receipts
type: chg
title: "Pair completion publication with explicit payload receipts"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-loom, sub-kernel-loom-pools, abi-loom-ring]
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libthyla-rs/src/loom.rs: SQE64/CQE16/params88 sizes and offsets unchanged; no private setup exposure"
  - "usr/lib/libthyla-rs/src/loom.rs: shared header offset constants unchanged; optional receipt geometry internal only"
depth: skeletal
---
Foundation26c21df87 passed50/50 clean boots across all five matrix rows.
New paired publication passes eight actual-source mutants/sanitizers, native
1830/1830 and pool-model clean/counterexample cases. CQ consumption cannot
return payload. Private owner/lifetime, ordered requests and client activation
remain owed; the foundation matrix does not qualify this later receipt code.
