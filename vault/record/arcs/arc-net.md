---
id: arc-net
type: arc
title: "The network arc (#68 charter: netd, /net, readiness, Weft)"
status: active
design: ["docs/NET-DESIGN.md", "docs/NET-THROUGHPUT.md"]
chunks:
  - chg-2026-06-18-net6b-poll-bridge
  - chg-2026-06-18-net6b4-close
  - chg-2026-06-21-294-cancel-at-close
follow-ons: [seam-221-idle-pump-wake, seam-223-pump-tail-starvation]
created: 2026-07-31
---
## Goal

The #68 network charter: netd (smoltcp over virtio-PCI), the /net 9P
surface, the readiness leg (dev9p.poll -- the arc's one kernel ABI), and
the Weft zero-copy dataplane that grew beside it.

## Planned chunks

HISTORICALLY COMPLETE (net-2a .. net-8 landed June 2026). Held `active`
while the Record backfill accretes this era's chunks -- the list above is
the vault-backfilled SUBSET (the dev9p.poll slice this sweep absorbed);
the full landed-chunk record is docs/NET-DESIGN.md section 20 + the phase
status docs + git log. Flips to `complete` when the netd/weft sweeps
finish this era's backfill.

## Close summary

(written at status flip to complete)
