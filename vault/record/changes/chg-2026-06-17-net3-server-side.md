---
id: chg-2026-06-17-net3-server-side
type: chg
title: "net-3: the server side (deferred accept) + UDP + ICMP + the net-3d close and loopback E2E"
date: 2026-06-17
arc: arc-net
commits: ["da7ffc5b", "943b72cc", "e013cfcb", "57630947"]
touched: [sub-netd-server, sub-netd-nic]
established: []
closed: [fnd-net3d-r1-f1, fnd-net3d-r1-f2]
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
net-3a (`da7ffc5b`): `announce` + the blocking `listen` via the DEFERRED
9P REPLY — the mechanism every later blocking semantic reuses — plus the
ninep Tflush/Rflush codec (a client dying on a blocked open cancels
cleanly). net-3b (`943b72cc`): UDP datagrams (the shared slot pool gains
the `proto` discriminator — the typed-get memory-safety axis). net-3c
(`e013cfcb`): ICMP ping (ident-bound echo sockets). net-3d (`57630947`):
the audit close — [[adt-net3d-r1]] (DIRTY: the F1 P1 strand class) →
[[adt-net3d-r2]] on the fix (converged clean) — plus the deterministic
in-guest loopback E2E (the isolated-stack design the smoltcp routing
proof forced: a lo iface sharing the NIC set mis-routes).
