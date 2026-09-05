---
id: fnd-net6b-r2-f1
type: fnd
title: "The >16-distinct-client case STARVES the tail, not merely delays it"
round: adt-net6b-r2
severity: P3
status: deferred
surface: [sub-kernel-ninep-dev9p-poll]
threatens: []
seam: seam-223-pump-tail-starvation
created: 2026-07-31
---
## Prosecution

The collect is a head-anchored LIFO scan with no rotation: past the cap,
the 16 nearest the head are pumped every cycle and a tail client's reply
is NEVER demuxed -- its pollers hang outright. Round 1 had described the
cap as graceful; this sharpening corrects it to starvation.

## Disposition

Deferred (unreachable below 17 simultaneous QTPOLL clients; v1.0 has 1).
The comments were tightened at the round; the v1.x per-client work-queue
MUST use a fair start. Tracked as task #223.
