---
id: fnd-rw4-rev2-f1
type: fnd
title: "RW-4 R2-F1: byte-mode /srv blocking recv extincts on a 2nd concurrent reader"
round: adt-rw4-r1
severity: P1
status: fixed
surface: [sub-kernel-srvconn]
threatens: [inv-i9]
hazard: haz-single-waiter-rendez
fixed-by: chg-2026-06-10-rw4-fixes
created: 2026-08-01
---
## Prosecution

The SrvConn ring's single-waiter Rendez was safe only under the 9P-mode
`p9_client.lock` serialization — but the P6-pouch-sockets BYTE-mode
userspace `read()` path has no serializer, and peer Threads share the
fd: two threads reading one conn fd trip
`extinction("rendez already has a waiter")` — an unprivileged,
SMP-reachable panic. Empirically latent (the SMP gate stayed green;
stratumd never trips it) — the textbook latent-P1 trap.

## Disposition

Fixed at `ee30f559`: a per-`srvconn_chan` `bool reading` single-reader
busy-guard (the devcons pattern) on `srvconn_client_recv` (s2c) +
`srvconn_server_recv_blocking` (c2s) — a second concurrent blocking
reader gets -1. The canonical [[haz-single-waiter-rendez]] instance for
the srv surface (same root class as RW-4 SA-F1: the multi-thread lift
outran per-Proc shared-state serialization). Note the later evolution:
CF-3B's #354 role-park REPLACED the refuse-with--1 posture for the
byte-I/O roles (a second party PARKS) while the rendezes stayed
single-waiter by construction — the guard's descendant, not its
reversal ([[sub-kernel-srvconn]] carries the as-built).
