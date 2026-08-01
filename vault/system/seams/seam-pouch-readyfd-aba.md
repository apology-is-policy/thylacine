---
id: seam-pouch-readyfd-aba
type: seam
title: "The lazy ready-fd open can bind the old connection's fd after slot reuse"
status: open
surface: [sub-pouch-net]
opened-by: chg-2026-06-18-net6b4-close
tracker: "#222"
created: 2026-08-01
updated: 2026-08-01
---
## Owed

`pouch_sock_poll_fd` drops [[lock-pouch-sock-table]] across the
`SYS_open` of `/net/<proto>/N/ready`, then re-acquires to publish. It
reconciles the two outcomes it can SEE (the slot went vacant; a peer won
the race) -- but not a close-and-REUSE of the same slot index in that
window, which would install the old connection's ready fd into a new
socket's slot. The slot table has no generation stamp.

Inside the inherited single-user-per-socket envelope
([[seam-pouch-sock-single-user]]) -- a program that closes a socket while
another thread polls it is already in POSIX-undefined territory -- which
is why it was P3 at the net-6b round, registered against a surface that
had no vault node yet.

## The lift

A per-slot generation counter for the WHOLE table (bumped at alloc), and
a re-check of it alongside `in_use` at every publish point. That also
retires the same class for `kernel_fd` and `ctl_fd`.
