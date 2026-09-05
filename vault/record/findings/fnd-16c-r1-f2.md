---
id: fnd-16c-r1-f2
type: fnd
title: "Steady-state ops over SrvConn run with no recv deadline"
round: adt-16c-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-transport]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

Twalk/Tread/Twrite via dev9p over the SrvConn transport block unboundedly
against a hung server.

## Disposition

Fixed AT THE ROUND: a defense-in-depth auto-arm in
`srvconn_transport_recv` (arm OP_DEADLINE when none set), refined at R2
(F1R2: also re-arm on a LAPSED deadline). **Superseded by #841**: the
auto-arm was removed wholesale -- under the pipelined elected reader a
per-op recv timeout abandons one in-flight op and desyncs the byte stream
every Proc shares (the stalk-3c root cause). Today's truth: caller-set
deadlines only (handshake bounded, steady state death-interruptible
unbounded). The finding's premise (bound a hung server) survives as the
untrusted-server seam family ([[seam-90-hung-server]]).
