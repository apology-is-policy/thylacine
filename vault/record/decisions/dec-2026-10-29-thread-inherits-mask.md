---
id: dec-2026-10-29-thread-inherits-mask
type: dec
title: "A new thread starts with its creator's note mask, from both spawn arms"
date: 2026-10-29
status: standing
decided-by: user-vote
affects: [sub-kernel-notes, sub-kernel-syscall-dispatch, sub-kernel-syscall-abi, sub-pouch-thread, sub-haul]
created: 2026-10-29
---
## Fork

C7 audit round 1 F1: the phenotype's clone did not copy the creator's
`note_mask`, and since C7 every first entry delivers notes before the entry's
first instruction. Native `SYS_THREAD_SPAWN` had the same exposure by documented
contract (a native thread started at zero, the rfork rule): a `pipe` queued while
every thread masks it was taken by the next newborn, which the Go runtime met at
every new M (`minit` makes a syscall before it masks `pipe`). Options: native
spawn inherits (POSIX `pthread_create`; Linux clone); keep the zero mask (the
hole stays: no native thread can mask before its first entry); an explicit mask
argument on `SYS_THREAD_SPAWN` (an ABI extension every caller must fill).

## Decision

Operator vote V5 (2026-10-29): inherit. Both arms copy the creating thread's
mask before the admit links the thread (ARCH 7.6.2). Pouch 0065 makes a new
thread's start restore its creator's mask (`__restore_sigs`), retiring 0007's
documented limitation; haul's pumps drop their own masking. The Go fork's minit
comment goes stale and changes with the fork's next move (shared fork).
