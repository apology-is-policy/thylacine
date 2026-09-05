---
id: lock-pouch-sock-table
type: lock
title: "g_lock — the pouch socket slot table (userspace)"
kind: spin (userspace; musl LOCK/UNLOCK)
guards: "the process-global pouch socket slot table — slot claim and vacate, state transitions, and every kernel_fd / ctl_fd / ready_fd write"
orders-before: []
created: 2026-08-01
updated: 2026-08-01
---
## What it protects

The process-global `g_table[POUCH_SOCK_MAX]` of pouch socket slots
(`src/network/_pouch_socket.c`) — every structural change: slot claim,
slot vacate, state transitions, and the writes of `kernel_fd` / `ctl_fd`
/ `ready_fd`. A userspace lock (musl's `LOCK`/`UNLOCK` spin), shared by
every pthread in the Proc.

## Order

A leaf, and the only pouch lock. It nests under nothing and nothing
nests under it — **including syscalls**: the lazy `ready`-fd open in
`pouch_sock_poll_fd` deliberately DROPS the lock across its `SYS_open`
and reconciles on re-acquire (slot closed meanwhile → close the orphan
and answer `EBADF`; a peer won the race → close ours and return the
winner's fd). Holding it across a blocking `/net` open would serialize
every socket operation in the Proc behind one 9P round-trip.

## Discipline

**Capture, then decide.** A resolver reads BOTH `in_use` and the fd it
wants INSIDE the critical section and sets `errno` from the locals;
re-reading `in_use` after the unlock lets a concurrent `close` flip it
and mis-categorize `EBADF` vs `ENOTCONN` (the sockets round's F8).

**Close after unlock.** `pouch_sock_close` captures the fds, marks the
slot vacant, unlocks, and only then issues `SYS_close`. The kernel
handle outlives the slot release for an instant, which is safe because a
peer can no longer observe the slot as live.

## Known gap

The lock covers the table's structure, not the socket's *state machine*.
`bind` / `connect` / `accept` inspect `slot->state` and write slot fields
through the resolved pointer WITHOUT re-acquiring — so two threads
racing `bind()` on one fresh fd can both see `FRESH`, both post, and the
second's write orphans the first's listener handle in the kernel handle
table. Every later patch's direct slot writes (the AF_INET fields, the
UDP lazy data-open, the `nonblock` flag) inherit that assumption. It is
the single-user-per-socket discipline, recorded as
[[seam-pouch-sock-single-user]] rather than a defect: POSIX itself leaves
concurrent same-fd socket setup undefined, and the fix is a CAS-style
transition helper.
