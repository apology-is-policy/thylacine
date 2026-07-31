---
id: fnd-p5srv-r1-f8
type: fnd
title: "A burst of hung handshakes can transiently exhaust SRV_MAX_CONNS"
round: adt-p5srv-r1
severity: P3
status: documented
surface: [sub-kernel-srvconn, sub-kernel-devsrv]
threatens: []
created: 2026-07-31
---
## Prosecution

`g_srvconn_created`/`g_srvconn_freed` balance on every path, but a burst
of failed-handshake connects holds live SrvConns for the handshake
deadline's duration — a window in which the global cap can refuse
legitimate connects.

## Disposition

Documented, no code: with the F1 deadline fix even a hung handshake
decays back to baseline within `SRVCONN_HANDSHAKE_DEADLINE_NS` (5 s),
and the era's per-Proc cap bounded each client to one conn. The per-Proc
cap was later REMOVED (stalk-3b — a session needs corvus AND its
stratum-fs concurrently), which widens this into the accepted
global-cap fairness tradeoff recorded at [[fnd-stalk3b-r1-f3]].
