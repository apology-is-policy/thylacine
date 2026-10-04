---
id: chg-2026-10-04-async-service-abi-reservation
type: chg
title: "Reserve Loom private service encoding before consumers"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [abi-loom-ring, sub-kernel-loom]
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libthyla-rs/src/loom.rs: Sqe / Cqe / BufReg / Params"
  - "usr/lib/libthyla-rs/src/loom.rs: HDR_SQ_HEAD .. HDR_FLAGS"
depth: skeletal
---
Under approved scripture4722f34e8, reserve setup bit2, register subops2..6 and
SQE20; existing19 stays reserved. Version1 record layouts and incarnation-tagged
source/destination slots are specified in ASYNC-SERVICE-ABI. Existing kernel/Rust
constants were inspected for collision; no C/Pouch/Go Loom service mirror exists
yet. New record mirrors and runtime code follow separately. Existing valid masks
remain unchanged. Documentation review is blind to actual decoder and lifecycle
behavior; no runtime qualification or activation is claimed.
