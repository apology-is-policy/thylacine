---
id: seam-240-lo-redial
type: seam
title: "netd: a lo-migrated UDP/ICMP slot re-dialed off-loopback silently drops"
status: open
surface: [sub-netd-server]
opened-by: adt-net8d-r1
tracker: "task #240"
created: 2026-07-31
updated: 2026-07-31
---
## Owed

The net-8a loopback migration is ONE-WAY: a UDP/ICMP slot whose first
dial was 127.x lives on the lo stack, and a RE-dial to a non-loopback
destination just updates `remote` (an already-open socket is not
re-migrated) — so `data_send` emits on the lo iface, which has no
route to the destination, and the datagram is silently dropped at
egress while the send reports success ([[fnd-net8d-r1-f1]]).

## What closes it

Either reject the cross-stack re-dial honestly (Err → EINVAL at the
ctl write) or re-migrate lo↔NIC on a destination-class change. Both
are netd-side; the reject is the minimal fix.

## Risk while open

Degradation-only on a narrow trigger (mixed loopback/non-loopback
destinations on ONE datagram socket — reachable via the pouch AF_INET
per-datagram `sendto` path; the native client is TCP-only). No panic,
no leak, no cross-connection effect; UDP is unreliable by contract.
