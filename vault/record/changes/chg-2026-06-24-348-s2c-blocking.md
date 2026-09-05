---
id: chg-2026-06-24-348-s2c-blocking
type: chg
title: "#348: the blocking s2c server send — back-pressure is not EPIPE"
date: 2026-06-24
arc: arc-go-build
commits: ["eacdf097"]
touched:
  - sub-kernel-srvconn
  - sub-kernel-devsrv
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The on-device go-build `snare:bus` root fix: stratumd's Rread replies
filled the s2c ring under the compile's concurrent-fault Tread burst,
the non-blocking `srvconn_server_send` returned 0, stratumd's
`write_full` treated 0 as EPIPE and CLOSED the kernel-attached mount
mid-build. New `srvconn_server_send_blocking` (parks on a SEPARATE
`s2c.wrendez` — each rendez keeps exactly one possible waiter, so the
single-waiter hazard is structurally unreachable) + the `client_recv`
drain-wake + the `writing` busy-guard + teardown's wrendez wakes;
`devsrv_write`'s server arm goes blocking. The s2c twin of
[[chg-2026-06-24-349-flow-control]] (same day, the c2s direction).
Regression `srvconn.server_send_blocks_then_drain_wakes` (non-vacuous).
Audit [[adt-348-r1]] CLEAN 0/0/1/3 — the P2 latent
([[fnd-348-r1-f1]]) became task #354, closed by
[[chg-2026-07-08-cf3b-bulk-ring]].
