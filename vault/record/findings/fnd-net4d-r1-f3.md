---
id: fnd-net4d-r1-f3
type: fnd
title: "The shared dns socket's queries Vec is a bounded reused high-water, not a leak"
round: adt-net4d-r1
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

smoltcp's growable query-slot table never shrinks — the Vec's
high-water sticks. Completed slots are REUSED (freed → None →
find_free_query), so it is not a leak; the high-water is bounded by
MAX_CONNS × MAX_FIDS concurrent in-flight queries.

## Disposition

Closed justified: documented as a [[sub-netd-server]] caveat beside
the ephemeral-port/ident notes.
