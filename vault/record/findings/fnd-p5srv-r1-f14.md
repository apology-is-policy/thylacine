---
id: fnd-p5srv-r1-f14
type: fnd
title: "Claimed missing poller wake on the connect failure paths"
round: adt-p5srv-r1
severity: P3
status: withdrawn
surface: [sub-kernel-devsrv]
threatens: []
created: 2026-07-31
---
## Prosecution

(As filed) The client-connect failure paths free the SrvConn without
waking registered pollers, stranding a waiter.

## Disposition

WITHDRAWN: both failure paths are correct — the early-bail path frees
the SrvConn before any poller could observe it; the handle-alloc failure
path frees immediately with no waiter holding a reference. A poller can
only register on an OBSERVABLE object. Recorded for the
do-not-re-report set.
