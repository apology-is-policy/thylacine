---
id: fnd-net3d-r1-f3
type: fnd
title: "The ICMP Echo-ident rotation is not liveness-checked (dup idents mis-route a reply)"
round: adt-net3d-r1
severity: P3
status: documented
surface: [sub-netd-server]
threatens: []
created: 2026-07-31
---
## Prosecution

`next_icmp_ident` rotates without checking liveness and smoltcp's
`bind(Ident)` permits duplicates — a wrapped collision (65536 clones in
one boot onto ≤16 live slots) mis-delivers an echo reply to the wrong
/net/icmp connection. Benign (mis-delivery, never a panic/UAF). (== the
self-audit's SA-1; the ephemeral-port parallel.)

## Disposition

Closed justified: the v1.x liveness-checked allocator, documented
in-code beside the port note; carried as a [[sub-netd-server]] caveat.
