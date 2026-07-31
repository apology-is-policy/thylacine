---
id: fnd-16c-r1-f1
type: fnd
title: "Handshake runs unbounded against a hung stratumd"
round: adt-16c-r1
severity: P1
status: fixed
surface: [sub-kernel-ninep-attach]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
created: 2026-07-31
---
## Prosecution

`sys_attach_9p_srv_handler` drives Tversion+Tattach with no recv deadline;
a hung stratumd blocks the caller indefinitely at boot.

## Disposition

Fixed: `srvconn_set_client_deadline(cn, now + SRVCONN_HANDSHAKE_DEADLINE_NS)`
immediately before `p9_attached_create`. Still true today (the handshake
half of the deadline split); the steady-state half was later inverted by
#841 (see [[fnd-16c-r1-f2]]).
