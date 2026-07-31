---
id: fnd-net2d-r1-f5
type: fnd
title: "The ephemeral-port rotation is not liveness-checked"
round: adt-net2d-r1
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

The 49152..=65535 rotation never checks whether the candidate port is
in use; a post-wrap same-4-tuple collision makes smoltcp reject the
connect (EINVAL — fail-closed, never a mis-delivery). (== the
self-audit's SF2; the ICMP ident rotation later drew the parallel note
at net-3d.)

## Disposition

Closed justified: documented in-code as the v1.x liveness-checked
allocator; the F3 peek-then-commit reduces the burn rate; the failure
mode is an honest error at astronomically-unlikely wrap collision
(MAX_SLOTS=16 concurrent).
