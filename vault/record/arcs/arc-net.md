---
id: arc-net
type: arc
title: "The network arc (#68 charter: netd, /net, readiness, Weft)"
status: active
design: ["docs/NET-DESIGN.md", "docs/NET-THROUGHPUT.md"]
chunks:
  - chg-2026-06-17-net2-netd-birth
  - chg-2026-06-17-net3-server-side
  - chg-2026-06-18-net4-cs-dns-ipifc
  - chg-2026-06-18-net6a-blocking-reads
  - chg-2026-06-18-net6b-poll-bridge
  - chg-2026-06-18-net6b4-close
  - chg-2026-06-19-net7b-summary
  - chg-2026-06-19-net8-resident-lo
  - chg-2026-06-21-294-cancel-at-close
  - chg-2026-06-21-netd-221-poll-cadence
  - chg-2026-06-21-netd-293-connect-sweep
  - chg-2026-07-22-52-nonblock
follow-ons: [seam-221-idle-pump-wake, seam-223-pump-tail-starvation, seam-220-netd-listener-poll, seam-56-netd-cancelled-tag, seam-240-lo-redial, seam-242-selftest-nonfatal, seam-netd-host-tests]
created: 2026-07-31
---
## Goal

The #68 network charter: netd (smoltcp over virtio-PCI), the /net 9P
surface, the readiness leg (dev9p.poll -- the arc's one kernel ABI), and
the Weft zero-copy dataplane that grew beside it.

## Planned chunks

HISTORICALLY COMPLETE (net-2a .. net-8 landed June 2026). Held `active`
while the Record backfill accretes this era's chunks -- the list above
now carries the netd-side era (the netd sweep's backfill: net-2 birth
through net-8, the #221/#293 fixes, the #52 nonblock surface) alongside
the dev9p.poll slice from the 9P-area sweep. Still pending: the net-1
netdev chunk, net-5 (the pouch boundary-line -- the pouch sweep), net-7a
(clock/SNTP/TLS -- their surfaces' sweeps), net-8c (tls/net-echo).
Flips to `complete` when those finish this era's backfill.

## Close summary

(written at status flip to complete)
