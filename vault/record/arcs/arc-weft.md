---
id: arc-weft
type: arc
title: "The Weft arc (#266): the capability network dataplane (I-37)"
status: active
design: ["docs/NET-THROUGHPUT.md"]
chunks:
  - chg-2026-06-20-weft0-payload-lift
  - chg-2026-06-20-weft6b-netd-drive
  - chg-2026-06-20-weft6c2-readiness-edge
  - chg-2026-06-21-weft7-close
created: 2026-07-31
---
## Goal

The per-flow zero-copy shared-Burrow dataplane over /net (I-37;
`specs/weft.tla` model-first): registration-is-the-capability, no
per-op mediation, grant-is-the-share delivery, the Loom data drive, and
the native `WeftFlow` push/pop/wait API.

## Planned chunks

HISTORICALLY COMPLETE (Weft-0 .. Weft-7 landed June 2026; the arc
CLOSED at Weft-7). Held `active` while the Record backfill accretes:
the list above is the NETD-SIDE slice the netd sweep absorbed
(Weft-1..6a kernel chunks — the spec, the syscalls, the Loom routing,
the dev9p fast paths — plus the #289 seam and the G-2 weave addendum
backfill with the kernel weft sweep). Flips to `complete` when that
sweep finishes this era.

## Close summary

(written at status flip to complete)
