---
id: fnd-349-self-sa1
type: fnd
title: "mark_dead missed the parked sender's wake (strand on death)"
round: adt-349-self
severity: P1
status: fixed
surface: [sub-kernel-ninep-client]
threatens: [inv-i9]
fixed-by: chg-2026-06-24-349-flow-control
regression: "9p_client.send_backpressure_multi_waiter (wake-all leg)"
created: 2026-07-31
---
## Prosecution

A sender parked in client_send_flow sleeps on the send-park mechanism, NOT
its rpc rendez -- so client_mark_dead_locked's per-rpc death-wake loop
missed it: a parked sender strands forever on a dead session.

## Disposition

Fixed (self-found, pre-formal-round): mark_dead -- verified the SOLE
c->dead setter -- also wakes the send-park side, so every death transition
a parked sender can observe reaches it.
