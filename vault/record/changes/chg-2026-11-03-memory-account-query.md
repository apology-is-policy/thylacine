---
id: chg-2026-11-03-memory-account-query
type: chg
title: "SYS_ACCOUNT answers its query on the caller's own account"
date: 2026-11-03
arc: arc-halcyon-interaction
commits: ["*(pending)*"]
touched: [sub-kernel-accounts, sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-devproc, sub-kernel-devctl, sub-kernel-protect-witness, sub-kernel-vivarium, abi-memory-account]
established: []
closed: []
opened: []
mirrors-checked:
  - kernel/include/thylacine/account.h
  - kernel/include/thylacine/syscall.h
  - usr/lib/libt/include/thyla/account.h
  - usr/lib/libt/include/thyla/syscall.h
  - usr/lib/libthyla-rs/src/account.rs
  - usr/lib/libthyla-rs/src/lib.rs
depth: skeletal
created: 2026-11-03
---
The reserved number gets its arm, with the query on the caller's own account as the only
thing behind it; the other operations answer -EOPNOTSUPP and any handle -EBADF. No record
layout or number changes, so the oracle's byte record is untouched. The kernel's and libt's
headers and libthyla-rs gain only comments and wrappers. `/proc/<pid>/account` renders the
same fill under sched's gate, and `/ctl/memory` gains two lines. Written and syntax-checked
while the mac was queued, not booted. The note's stale sentence about rt_sigreturn is corrected
too: [[chg-2026-11-03-memory-account-number]] moved the number and left it behind.
