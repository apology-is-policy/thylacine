---
id: chg-2026-10-04-async-buffer-pool-design
type: chg
title: "Separate private streaming payload leases from completion consumption"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-loom, abi-loom-service]
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libt/include/thyla/loom_service.h: five records and constants"
  - "usr/lib/libthyla-rs/src/loom/service_abi.rs: five repr(C) records and constants"
depth: skeletal
---
# Operator-selected provided-buffer pools

The operator chose C after AS-R7 exposed the missing per-shot buffer handoff.
ASYNC-SERVICE-BUFFERS.md reserves full-width companion receipts, explicit return,
bounded pools and peer-independent cancellation. Existing scalar multishot stays
unchanged. AS-R8 queues the raw Rust registration safety repair. This is design,
not a live private handler or clipboard activation.

Separately, the prerequisite engine at7571ad4e4 passed50/50 clean five-row boots,
with all failure classifications zero and exact source/index/draft verification.
Evidence: work/oct4-async-service/as2-matrix/verified.json. Single-agent review.

Existing mirrors were inspected for collisions and remain unchanged here; new
compiled declarations follow separately. The first render refused this change
note for missing mirror-check metadata; corrected before commit.
