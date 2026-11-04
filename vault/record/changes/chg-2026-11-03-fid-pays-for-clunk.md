---
id: chg-2026-11-03-fid-pays-for-clunk
type: chg
title: "A FID pays for its CLUNK at RESERVE"
date: 2026-11-03
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-loom, sub-loom-native-adapter]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-11-03
---
Operator vote 51. A private CLUNK is no longer refused for memory: a FID's RESERVE charges
and allocates the page, and the CLUNK runs on it. The budget now refuses the RESERVE, which
both adapters already return as an ordinary error, and vote 11's premise holds for memory.
Not booted; the in-kernel legs and their sabotage are owed under the next mac lease.
