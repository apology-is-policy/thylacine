---
id: fnd-net8d-r1-f1
type: fnd
title: "A lo-migrated UDP/ICMP slot re-dialed off-loopback silently drops at lo egress while reporting success"
round: adt-net8d-r1
severity: P3
status: deferred
surface: [sub-netd-server]
threatens: []
seam: seam-240-lo-redial
created: 2026-07-31
---
## Prosecution

The net-8a migration is ONE-WAY: a datagram slot whose first dial was
127.x lives on the lo stack; a re-dial to a non-loopback destination
just updates `remote` (an already-open socket is not re-migrated), so
`data_send` emits on the lo iface — no route — and the datagram drops
silently while `send_slice` reports `data.len()`. Confirmed real from
the code; no panic (handle+set stay consistent), no leak, no privilege
effect; narrow trigger (mixed destinations on ONE socket, reachable
via the pouch per-datagram sendto path — the native client is
TCP-only). The prosecutor's catch; the self-audit missed it.

## Disposition

Deferred → [[seam-240-lo-redial]] (task #240): reject the cross-stack
re-dial honestly or re-migrate. Tracked rather than fixed at the clean
close (it touches the just-audited data path; degradation-only; the
net-arc precedent for narrow P3 latents).
