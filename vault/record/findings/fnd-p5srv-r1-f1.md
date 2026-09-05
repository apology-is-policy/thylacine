---
id: fnd-p5srv-r1-f1
type: fnd
title: "Production /srv ops never armed client_deadline_ns — a hung server wedged its caller indefinitely"
round: adt-p5srv-r1
severity: P1
status: fixed
surface: [sub-kernel-srvconn, sub-kernel-devsrv]
threatens: []
fixed-by: chg-2026-05-19-srv-birth
regression: "srv_client.handshake_times_out_without_responder (era test; the suite migrated at stalk-3c — the boot E2E carries the production path since)"
created: 2026-07-31
---
## Prosecution

The kernel-internal tests correctly set `client_deadline_ns` before each
blocking op; the PRODUCTION path (`sys_srv_connect_for_proc` and both
KOBJ_SRV r/w arms) skipped it. With the default 0 every blocking
`tsleep` read "no deadline", so a corvus that hung — no crash, no EOF —
wedged joey (or any client) forever on a `/srv` op.

## Disposition

Fixed in the audit-close commit: `srvconn_set_client_deadline` armed
before the handshake (`SRVCONN_HANDSHAKE_DEADLINE_NS` = 5 s) and before
each steady-state op (`SRVCONN_OP_DEADLINE_NS` = 30 s — sized to
corvus's Argon2id + AEGIS + ML-KEM verb budget on emulated targets);
the two constants landed in `srvconn.h`. Historical note: the
steady-state auto-arm was itself later REVERSED for the shared kernel 9P
client at #841 (a per-op recv timeout on a pipelined shared stream
desyncs it — see [[sub-kernel-ninep-transport]] Caveats); the
handshake-arming discipline survives.
