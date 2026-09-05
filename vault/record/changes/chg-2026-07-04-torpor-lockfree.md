---
id: chg-2026-07-04-torpor-lockfree
type: chg
title: "The R-5 pre-fault + the #343 lock-free mismatch return"
date: 2026-07-04
arc: arc-go-build
commits: ["48fbd91d"]
touched: [sub-kernel-torpor]
established: []
closed: []
opened: [seam-torpor-reclaim-uaccess]
mirrors-checked: []
depth: rich
---
## What

Two torpor changes in one commit (shared with the REVENANT read-ahead
work), each closing a different class:

**The R-5 F1 pre-fault.** REVENANT made text pages file-backed, so
the under-lock `uaccess_load_u32` could newly reach a BLOCKING 9P
demand-page — a sleep under the GLOBAL `torpor_lock`, i.e. a
system-wide futex stall (permanent under a wedged FS). The word is
now faulted in BEFORE the lock; the under-lock reload can only
re-fault into the non-blocking lazy-anon arm (decommit-race window)
or -EFAULT cleanly. The property is a standing obligation on any
future reclaim pass ([[seam-torpor-reclaim-uaccess]]).

**The #343 mismatch fast path.** Measured: 36.8 M of a go build's
67.7 M `torpor_wait` calls are osyield's
`wait(&sleepDummy, 1, tiny)` — ALL on one address, therefore one
bucket, therefore unshardeable. The fix is the Linux
compare-before-queue shape: `prefault != expected` returns
`TORPOR_OK` without ever taking `torpor_lock`. Sound because no
waiter registers on that path ([[inv-i9]]'s window opens only
between register and sleep); the deliberately-weakened ordering
(plain load, no incidental acquire) is covered by the universal
futex re-check contract, verified against all three consumers (musl,
Go runtime, libthyla-rs).

The audit close (0/0/0/5) rode the same commit.
