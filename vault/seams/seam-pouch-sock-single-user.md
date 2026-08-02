---
id: seam-pouch-sock-single-user
type: seam
title: "Socket state transitions are not locked — one fd, one user"
status: open
surface: [sub-pouch-net]
opened-by: chg-2026-05-23-p6-sockets
tracker: "sockets-12 F4"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`pouch_sock_resolve` returns a slot pointer under
[[lock-pouch-sock-table]], but the post-resolve state inspection
(`FRESH`/`LISTENING`/`CONNECTED`) and the slot-field writes happen
WITHOUT re-acquiring. Two pthreads concurrently calling `bind()` on one
fresh fd can each see `FRESH`, each post a service, and the second's
write orphans the first's listener handle in the kernel handle table.

Every later patch inherits the assumption: net-5's `connect`/`accept`
slot writes, net-6a-2's UDP lazy data-open, #52's `nonblock` flag. The
one place it was reconciled is the lazy `ready`-fd open (which had to
drop the lock across a syscall and therefore HAD to handle the race).

No exposure at v1.0 — the provers use one slot per thread — and POSIX
itself leaves concurrent same-fd socket setup undefined.

## The lift

A CAS-style transition helper: compare-and-set the state and publish the
fd under one lock hold, so a loser sees the winner's state instead of
overwriting it.
