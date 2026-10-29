---
id: chg-2026-10-04-async-buffer-pool-abi
type: chg
title: "Pin provided-buffer records in three compiled mirrors"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-loom, sub-kernel-syscall-abi, sub-libthyla-rs, abi-loom-service]
established: []
closed: []
opened: []
mirrors-checked:
  - "usr/lib/libt/include/thyla/loom_service.h: ten records and constants"
  - "usr/lib/libthyla-rs/src/loom/service_abi.rs: ten repr(C) records and constants"
depth: skeletal
---
Under scripture30695b43e, kernel/native C and Rust reserve30 constants/10 records
with full incarnation/lease identity, fixed completion envelope and private masks
disabled. Independent serialized vectors, ARM64 layout/full-header guards and
three named mirror mutations pass. First stale-SDK compile failure is preserved;
corrected host invocation uses the actual SDK without global changes.

The pool model and consumers are next. No native runtime, sanitizer or graphics
qualification is claimed. Existing mirror files are adopted into the owning C
ABI and Rust runtime dossiers. AS-R8 raw registration safety remains an owned
client requirement. Evidence: work/oct4-async-service/buffer-pools/abi-passed.json.
