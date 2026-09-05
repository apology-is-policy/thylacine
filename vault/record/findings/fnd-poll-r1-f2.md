---
id: fnd-poll-r1-f2
type: fnd
round: adt-poll-r1
severity: P2
status: fixed
title: "Teardown latched the two EOF flags under separate locks — a poll between saw half a hangup"
surface: [sub-kernel-poll]
threatens: [inv-i9]
fixed-by: chg-2026-05-20-p5-poll
regression: "the conn-teardown poll test asserts the flag pair is observed together"
created: 2026-08-01
---
## Prosecution

`srvconn_teardown` set `c2s.eof` then `s2c.eof` via two separate
`chan_set_eof` calls, each under only its own channel lock — while
`srvconn_poll` samples under BOTH locks together. A poll landing
between the two latches observed `c2s.eof = true, s2c.eof = false`:
POLLHUP set, POLLERR not — a half-hangup state no producer intended
and the header's semantics table did not admit.

## Fix

The teardown latches both flags before the readiness edge is
published to pollers, so a sample sees none-or-both.
