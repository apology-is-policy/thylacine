---
id: fnd-net3d-r1-f4
type: fnd
title: "A slot-table-full deferred accept buffers the inbound call indefinitely"
round: adt-net3d-r1
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

When `accept_swap` finds no free slot, the pending stays registered and
the established call sits buffered in the listener's socket until a
slot frees — a liveness (not safety) property, bounded by
MAX_PENDING_ACCEPTS=16.

## Disposition

Closed justified: documented as the #65 resource-floor behavior (the
call retries every poll; nothing leaks); the bound is the design.
