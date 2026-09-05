---
id: chg-2026-06-24-355-poll-decouple
type: chg
title: "The fd table grows 64→256; poll's bound deliberately does not follow"
date: 2026-06-24
arc: arc-go-build
commits: ["ffcc64b7"]
touched: [sub-kernel-poll]
established: []
closed: []
opened: [seam-poll-heap-waiters]
mirrors-checked: []
depth: skeletal
---
## What

`PROC_HANDLE_MAX` 64→256 (kmalloc-backed) for the go toolchain's fd
appetite — and the poll bound CUT from the identity: `POLL_MAX_NFDS`
stays 64 because `sys_poll_for_proc`'s `waiters[]` + `held[]` are
stack frames (~14 KiB at 256 would breach the kstack). The absorbed
reference doc and `syscall.h`'s SYS_POLL comment both still teach
the old identity; the handler code is correct
([[sub-kernel-poll]]'s caveat). Lifting past 64 is
[[seam-poll-heap-waiters]].
