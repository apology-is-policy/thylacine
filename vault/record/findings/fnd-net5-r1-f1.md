---
id: fnd-net5-r1-f1
type: fnd
round: adt-net5-r1
severity: P2
status: fixed
title: "shutdown / sendto / recvfrom were not tag-aware — ENOSYS on an AF_INET socket"
surface: [sub-pouch-net]
threatens: []
fixed-by: chg-2026-06-18-net6a2-datacalls
regression: "the `/pouch-hello-net` data-call control surface"
created: 2026-08-01
---
## Prosecution

0006 never overrode `shutdown`, `sendto`, `recvfrom`, `sendmsg`, or
`recvmsg`; net-5 materially WIDENS the gap by making AF_INET TCP sockets
reachable, where `shutdown(SHUT_WR)` is a core idiom (send a request,
half-close, read the response).

Fail-closed: the tag bits never reach a real kernel syscall, so the
result is a loud `ENOSYS`, not a garbage fd — which is what keeps it P2.

## Disposition, then fix

DOCUMENTED at net-5 and phased to net-6, deliberately: the read path was
still non-blocking (a 0-return was ambiguous between no-data-yet and
EOF), so a usable TCP client could not work at net-5 REGARDLESS, and
building `shutdown` over a broken read path would be a half-feature. The
v1.0 claim was narrowed instead.

FIXED at net-6a-2, once netd's data read blocked: `shutdown` -> the
`hangup` ctl verb, `sendto` with a per-datagram UDP dest, `recvfrom` with
a best-effort `src` fill. `sendmsg`/`recvmsg`/`socketpair` stay
`ENOSYS` on the same reasoning ([[seam-pouch-sendmsg]]).

The two-step is the point: a fail-closed gap phased to the sub-chunk that
can close it properly is honestly closed by narrowing the documented
claim, not by building half of it early.
