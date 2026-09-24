---
id: chg-2026-09-24-pty-interaction-abi
type: chg
title: "Reserve the kernel-backed terminal ownership ABI"
date: 2026-09-24
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch]
established: [abi-pty-interaction]
closed: []
opened: []
mirrors-checked: [abi-pty-interaction]
depth: skeletal
created: 2026-09-24
---
Reserve SYS_PTY_REGISTER suboperations 16..21 and the exact 80/24-byte records
in kernel C, libt C and Rust before consumers. All offsets/alignment are pinned;
binding IDs cannot cross INT64_MAX and alias negative syscall errors. The
compiled three-language fixture passes against a literal 200-byte oracle on
Linux/AArch64. Initial staging lacked kernel headers and failed before any
fixture compiled; both logs are retained. No dispatch, live ownership or poll
behavior is implemented by this checkpoint. The operator approved the scope;
review was single-agent, not an independent audit.
