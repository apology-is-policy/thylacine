---
id: chg-2026-07-22-52-nonblock
type: chg
title: "#52 (TyrQuake multiplayer): the BSD nonblocking-socket surface — the netd nonblock verb + E_AGAIN"
date: 2026-07-22
arc: arc-net
commits: ["67b72e66"]
touched: [sub-netd-server]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-07-31
---
TyrQuake's `UDP_OpenSocket` sets `ioctl(FIONBIO)`, which had no pouch
surface — multiplayer was dead at "Unable to open control socket". The
design IS the fix: a nonblocking read is try-and-EAGAIN, never
poll-then-read, and netd owns it. The netd half: a per-connection
`nonblock` flag (the `nonblock 1/0` ctl verb); the FK_DATA WouldBlock
arm answers `E_AGAIN` (→ the guest's EWOULDBLOCK) instead of parking a
PendingRead — so the read path never touches the readiness bridge. The
rejected alternative (an earlier pouch cut gating each read on a
0-timeout poll) churned the shared session's tag pool to EXHAUSTION
(each 0-timeout poll parks a probe the kernel kthread Tflush-abandons;
`awaiting_flush` tags piled up faster than netd Rflushed → spurious EIO
on every read) — root-caused with a kernel branch tracer, then designed
around; the kernel stayed byte-unchanged. FIONREAD answers the 1/0
truthiness contract via a documented cold-path bridge probe;
SO_BROADCAST joins the pouch shim. The pouch-0025 half backfills with
the pouch sweep; the TyrQuake arc's own record with its sweep.
