---
id: fnd-sockets12-r1-f1
type: fnd
round: adt-sockets12-r1
severity: P1
status: fixed
title: "A server-side read on a byte-mode SrvConn returned EOF racing the client's first write"
surface: [sub-pouch-net]
threatens: []
fixed-by: chg-2026-05-23-p6-sockets
regression: "`srv_client.byte_mode_server_recv_blocking_eof`"
created: 2026-08-01
---
## Prosecution

1. `srvconn_server_recv` (corvus's poll-then-read pattern) is
   non-blocking: `chan_consume_nonblock` returns 0 when the ring is empty
   and EOF is not latched.
2. The pouch AF_UNIX server thread's accept-wake races the client's first
   `write()` across SMP CPUs.
3. The server's `read()` can enter the consume BEFORE the client's send
   lands, and sees 0.
4. POSIX says a 0-return on a stream socket is EOF. The server closes a
   connection that had not started.

## Fix

`srvconn_server_recv_blocking` — the mirror of the client-side recv
against the `c2s` ring, tsleeping on its Rendez until data arrives or EOF
is latched. `devsrv_read` dispatches on `cn->byte_mode`, so 9P-mode's
poll-then-read pattern is untouched. The regression covers the finite
case; the empty-but-live blocking case needs multi-threaded test infra
and rests on the prover.
