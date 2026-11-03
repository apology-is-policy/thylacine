---
id: dec-2026-11-02-postnote-tid
type: dec
title: "SYS_POSTNOTE_TID is native syscall 138"
date: 2026-11-02
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-abi, sub-kernel-syscall-dispatch, sub-kernel-notes, sub-pouch-thread]
created: 2026-11-02
---
## Fork

Thread-directed notes need a native entry, since `tkill` and `tgkill` serve only the Linux phenotype and Pouch's `pthread_kill` is native. Options: a new syscall; a thread selector on `SYS_POSTNOTE`, which every caller of that entry would then fill.

## Decision

Vote 14 of 2026-11-02: `SYS_POSTNOTE_TID` = 138 `(tid, name_va, name_len)`; tid 0 is the calling thread; own Proc only.
