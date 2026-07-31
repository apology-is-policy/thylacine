---
id: chg-2026-06-18-net6a-blocking-reads
type: chg
title: "net-6a: blocking data reads (RecvOutcome) + the native net API / echo server"
date: 2026-06-18
arc: arc-net
commits: ["43a5dae2", "934865e0"]
touched: [sub-netd-server, sub-netd-nic]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
net-6a-1 (`43a5dae2`): `data_recv_outcome` (WouldBlock/Data/Eof —
grounded in smoltcp's recv_error_check) + the `PendingRead` park /
`poll_data` delivery, closing the net-5 F2 recv-returns-0 ambiguity: a
pouch `recv()` blocks with no shim change. The SA-1 self-catch: a
0-count read returns at once (a 0-length dequeue reads as WouldBlock and
would park forever). net-6a-3 (`934865e0`): the native
`libthyla_rs::net` TcpStream/TcpListener + `usr/net-echo` + netd's
`echo_e2e` (the ≥2-concurrent accept + bidirectional echo server-logic
proof); the libthyla-rs/net-echo halves backfill with their own sweeps.
