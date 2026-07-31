---
id: fnd-16c-r1-f9
type: fnd
title: "Dead short-write branch in srvconn_transport_send"
round: adt-16c-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-transport]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

A partial-accept branch that could not execute (the ring cap
static_assert guarantees full-frame accepts for kernel-client sends)
invited a future maintainer to rely on it.

## Disposition

Fixed: simplified away. (The #349 EAGAIN evolution later gave the send
path its real three-way contract: full / zero-with-EAGAIN / fatal.)
