---
id: fnd-p5srv-r1-f10
type: fnd
title: "client_deadline_ns defaults to 0 — unsafe-by-default for any future blocking caller"
round: adt-p5srv-r1
severity: P3
status: documented
surface: [sub-kernel-srvconn]
threatens: []
created: 2026-07-31
---
## Prosecution

`srvconn_create` initializes the deadline to 0 ("no deadline"), so any
NEW blocking-op site that forgets to arm it silently inherits an
unbounded wait — exactly the F1 class, waiting to recur.

## Disposition

Documented: the default-0 is retained deliberately as the
tests-set-it-directly hook; the discipline is set-before-op at every
production site (F1's fix). Post-#841 the discipline INVERTED for the
shared kernel client's steady state — deadline-0 is now the CORRECT
posture there (block until reply/EOF/death; a per-op timeout desyncs the
shared stream), with the handshake still armed. The caveat lives on
[[sub-kernel-srvconn]]; the sticky-`client_timed_out` note rides with it.
