---
id: adt-sockets12-r1
type: adt
title: "P6-pouch-sockets round 1"
date: 2026-05-23
scope: [sub-pouch-net]
reviewer: opus
model-start: "opus"
model-end: "opus"
verdict: clean
counts: {p0: 0, p1: 2, p2: 2, p3: 9}
findings: [fnd-sockets12-r1-f1, fnd-sockets12-r1-f2, fnd-sockets12-r1-f8, fnd-sockets12-r1-f11]
round-of: chg-2026-05-23-p6-sockets
created: 2026-08-01
---
Focused opus prosecutor over the new kernel byte-mode SrvConn transport,
the boundary-line patch, and the proving binary. The kernel surface was
audited deeply; the userspace patch was read for shape and prosecuted for
signatures -- which the round says plainly, and which is why both P1s
landed on the kernel half.

Its own note on scope is worth keeping: "audited but not exhaustively:
the 13 musl files -- read for shape, prosecuted for sigs, but not every
musl POSIX-conformance corner."
