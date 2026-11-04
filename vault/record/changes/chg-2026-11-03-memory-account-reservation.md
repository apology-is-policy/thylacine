---
id: chg-2026-11-03-memory-account-reservation
type: chg
title: "Reserve the memory account call and its records"
date: 2026-11-03
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-vivarium]
established: [abi-memory-account]
closed: []
opened: []
mirrors-checked: [abi-memory-account]
depth: skeletal
created: 2026-11-03
---
MM-0's reservation under votes 47 and 48. SYS_ACCOUNT takes 139 with no dispatch arm, so
it answers as an unknown number, and the gap pin says so. The ceiling rises over
rt_sigreturn, argued per number. Kernel C, libt C and Rust carry the query and the outcome,
held to one hand-packed record by a new test. Sabotage of that test found its first form
blind to a swap of two like fields, since its C fixture was positional, and blind to a
swap of flags and pad, both zero; it now names its fields and fills them all. Nothing
behaves differently, and no consumer exists.
