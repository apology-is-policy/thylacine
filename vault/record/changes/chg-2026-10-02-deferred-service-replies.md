---
id: chg-2026-10-02-deferred-service-replies
type: chg
title: "Park and cancel bounded service replies"
date: 2026-10-02
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-halcyond-service-wire]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
The transport permits one explicit parked reply with a monotone local ticket.
Input and flush continue; exact resumption checks precede output construction.
Cancellation discards unsent output/input, and partial frames permanently close
the connection. Media adapters retain immediate serialization. Interaction exposes
drain-required state even after its application completion has been cancelled.

Actual-source schedules, named mutations and real SrvConn evidence are recorded
in HALCYON-INTERACTION-STATUS. This does not activate clipboard dispatch or itself
satisfy the full application-cache/SAK retirement barrier. Single-agent review,
October 2 gate waiver and protected draft preservation remain in force.
