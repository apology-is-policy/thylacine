---
id: fnd-cf3b-r1-f1
type: fnd
title: "The blocking client send deferred its POLLIN edge to end-of-delivery — a circular wait against a poll-then-read server"
round: adt-cf3b-r1
severity: P1
status: fixed
surface: [sub-kernel-srvconn]
threatens: [inv-i9]
fixed-by: chg-2026-07-08-cf3b-bulk-ring
regression: srvconn.client_send_blocking_poll_edge
created: 2026-07-31
---
## Prosecution

`srvconn_client_send_blocking` woke the conn's poll list ONCE at
end-of-delivery; the non-blocking twin it replaced woke per write. A
poll-then-read byte server (POSIX-standard: `poll(POLLIN, -1)` on the
accepted conn Spoor, whose server-endpoint POLLIN is c2s non-empty) plus
a client blocking-write LARGER than the ring is a circular wait: the
client parks on `c2s.wrendez` needing the drain; the server parks in
poll() needing an edge that only fires after the delivery it was
supposed to enable. A genuine regression OF this chunk — latent only
because the boot chain's byte servers happen to blocking-read.

## Disposition

Fixed in-commit: `poll_waiter_list_wake(&cn->poll_list)` fires on EVERY
accepted chunk inside the delivery loop (the per-write discipline of the
non-blocking twin); the end-of-function wake dropped as redundant.
Regression: a poller parked on the empty ring must wake WHILE the >cap
send is still in flight — on pre-fix code it stays SLEEPING and the
assert fails (no hang).
