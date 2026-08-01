---
id: fnd-poll-r1-f1
type: fnd
round: adt-poll-r1
severity: P2
status: fixed
title: "A client polling its own srv connection got the SERVER endpoint's revents"
surface: [sub-kernel-poll]
threatens: [inv-i9]
fixed-by: chg-2026-05-20-p5-poll
regression: "the devsrv-conn poll tests pin both endpoint views"
created: 2026-08-01
---
## Prosecution

`srv_handle_poll`'s SrvConn arm delegated straight to `srvconn_poll`,
whose revents are SERVER-endpoint semantics: POLLIN ⇔ c2s has bytes
(what the server reads), POLLOUT ⇔ s2c has room (where the server
writes). A CLIENT polling its own connection handle needs the mirror
image — POLLIN ⇔ s2c has bytes. A client select-loop would have
woken on its own outbound data and slept through inbound.

## Fix

Endpoint-aware revents: the dispatch discriminates which side of the
rings the polled handle is, and mirrors accordingly.
