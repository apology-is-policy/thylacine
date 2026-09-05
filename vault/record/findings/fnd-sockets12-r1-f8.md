---
id: fnd-sockets12-r1-f8
type: fnd
round: adt-sockets12-r1
severity: P3
status: fixed
title: "pouch_sock_kernel_fd read in_use again outside the lock, mis-categorizing errno"
surface: [sub-pouch-net]
threatens: []
fixed-by: chg-2026-05-23-p6-sockets
created: 2026-08-01
---
## Prosecution

The helper captured `kernel_fd` inside the critical section but then read
`g_table[i].in_use` AGAIN after the unlock to decide which errno to
report. A parallel `close()` racing the unlock flips `in_use` 1 -> 0, so
the caller sees `EBADF` where `ENOTCONN` was correct (or the reverse).

User-facing weirdness, no data harm — but the pattern is the one that
matters: a locked read followed by an unlocked re-read of the same state
is not a locked read.

## Fix

Capture BOTH `in_use` and `kernel_fd` into locals inside the lock; set
errno from the locals. Now the discipline of
[[lock-pouch-sock-table]].
