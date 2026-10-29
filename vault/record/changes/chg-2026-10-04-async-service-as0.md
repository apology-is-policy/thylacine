---
id: chg-2026-10-04-async-service-as0
type: chg
title: "Pin the private service ABI and cancellation model"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [abi-loom-ring, abi-loom-service, sub-kernel-loom, spec-loom-service]
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libthyla-rs/src/loom.rs: Sqe / Cqe / BufReg / Params"
  - "usr/lib/libthyla-rs/src/loom.rs: HDR_SQ_HEAD .. HDR_FLAGS"
  - "usr/lib/libt/include/thyla/loom_service.h: five records and constants"
  - "usr/lib/libthyla-rs/src/loom/service_abi.rs: five repr(C) records and constants"
depth: skeletal
---
After scripture4722f34e8 and reservationd2362ec11: three actual compiled mirrors,
22 constants, five records and all offsets agree with independent vectors.
Three source-mirror mutations are detected. Full kernel header ARM64 assertions
prove private setup and opcode dispatch remain disabled. Lifecycle model:
5,828 states and seven named counterexamples, including peer-independent local
retirement. Single-agent review in ASYNC-SERVICE-SELF-REVIEW.md; runtime locks,
authority and framing are not qualified by these fixtures. No new broad runtime
or graphical claim. Protected drafts remain separate; no Main landing.
